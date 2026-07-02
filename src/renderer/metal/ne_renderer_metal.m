#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_pass.h"
#include "ne_renderer_pipeline.h"
#include "ne_renderer_shader.h"
#include "ne_window.h"

#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

enum {
    NE_MTL_MAX_FRAMES_IN_FLIGHT    = 2,
    NE_MTL_POOL_INITIAL_CAP        = 16,
};

/* ── Resource pool slot types ───────────────────────────────────────────── */

typedef struct NEShaderSlot {
    bool occupied;
    NEShaderStage stage;
    void *library;     /* id<MTLLibrary>  */
    void *function;    /* id<MTLFunction> */
    char *entry_point; /* strdup'd copy   */
} NEShaderSlot;

typedef struct NEBufferSlot {
    bool occupied;
    bool dynamic;          /* true: per-frame copies, safe to update while GPU reads another */
    uint32_t usage;
    uint32_t size;
    void *copies[NE_MTL_MAX_FRAMES_IN_FLIGHT]; /* id<MTLBuffer>; dynamic uses all, static uses [0] */
} NEBufferSlot;

/* Copies a buffer slot holds: one per in-flight frame if dynamic, else one. */
static inline uint32_t ne_buffer_slot_copy_count(const NEBufferSlot *slot) {
    return slot->dynamic ? (uint32_t)NE_MTL_MAX_FRAMES_IN_FLIGHT : 1u;
}

typedef struct NEPipelineSlot {
    bool occupied;
    void *pipeline_state;  /* id<MTLRenderPipelineState> */
    uint32_t topology;     /* MTLPrimitiveType value     */
} NEPipelineSlot;

/* ── Renderer ───────────────────────────────────────────────────────────── */

struct NERenderer {
    void *device;
    void *queue;

    NEShaderSlot *shaders;
    uint32_t shader_count;
    uint32_t shader_cap;

    NEBufferSlot *buffers;
    uint32_t buffer_count;
    uint32_t buffer_cap;

    NEPipelineSlot *pipelines;
    uint32_t pipeline_count;
    uint32_t pipeline_cap;

    NEShaderOptimization shader_optimization; /* default: NE_SHADER_OPTIMIZATION_NONE (0) */
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    void *layer;          /* CAMetalLayer*              */
    void *drawable;       /* id<CAMetalDrawable>        */
    void *command_buffer; /* id<MTLCommandBuffer>       */
    void *encoder;        /* id<MTLRenderCommandEncoder> — active during frame */

    float clear_color[4];

    /* Frame pacing: limits how far the CPU can get ahead of the GPU. */
    dispatch_semaphore_t frame_semaphore;

    /*
     * Per-surface frame ring index (0 .. NE_MTL_MAX_FRAMES_IN_FLIGHT-1),
     * advanced once per begin_frame. Selects which copy of a dynamic buffer to
     * write/bind this frame. Each surface paces independently, so the index
     * lives on the surface, not the renderer — this is what lets N surfaces
     * render on independent timelines.
     */
    uint32_t frame_index;

    /* Per-pass draw state (valid between begin_frame / end_frame). */
    uint32_t current_topology;        /* MTLPrimitiveType              */
    NEBufferHandle current_index_buffer;
    uint32_t current_index_type;      /* MTLIndexType                  */
};

/**
 * NERenderPass is a lightweight token pointing back to the owning surface.
 * A single static instance is reused each frame to avoid per-frame heap
 * allocation.  Only one frame may be in-flight at a time — begin_frame will
 * reject a second call until end_frame clears the active pass.
 */
struct NERenderPass {
    NERenderSurface *surface;
};

static NERenderPass g_active_pass = {0};

/* Association key used to enforce one render surface per window. */
static const void *g_surface_assoc_key = &g_surface_assoc_key;

static CAMetalLayer *ne_surface_get_layer(const NERenderSurface *surface) {
    return surface && surface->layer ? (__bridge CAMetalLayer *)surface->layer : nil;
}

static id<CAMetalDrawable> ne_surface_get_drawable(const NERenderSurface *surface) {
    return surface && surface->drawable ? (__bridge id<CAMetalDrawable>)surface->drawable : nil;
}

static id<MTLCommandBuffer> ne_surface_get_command_buffer(const NERenderSurface *surface) {
    return surface && surface->command_buffer ? (__bridge id<MTLCommandBuffer>)surface->command_buffer : nil;
}

/* ── Generic pool alloc helper ──────────────────────────────────────────── */

/**
 * Finds a free slot in a pool or grows it.  Works for any slot type whose first
 * field is `bool occupied`.  Returns the slot index, or UINT32_MAX on failure.
 */
static uint32_t ne_pool_alloc(void **pool_ptr, uint32_t *cap_ptr, size_t slot_size) {
    uint8_t *pool = (uint8_t *)*pool_ptr;
    uint32_t cap = *cap_ptr;

    for (uint32_t i = 0; i < cap; i++) {
        bool *occupied = (bool *)(pool + i * slot_size);
        if (!*occupied) {
            return i;
        }
    }

    uint32_t new_cap = cap == 0 ? NE_MTL_POOL_INITIAL_CAP : cap * 2;
    void *new_pool = realloc(*pool_ptr, new_cap * slot_size);
    if (!new_pool) {
        return UINT32_MAX;
    }
    memset((uint8_t *)new_pool + cap * slot_size, 0, (new_cap - cap) * slot_size);

    uint32_t index = cap;
    *pool_ptr = new_pool;
    *cap_ptr = new_cap;
    return index;
}

static void ne_shader_slot_release(NEShaderSlot *slot) {
    if (slot->function) {
        (void)CFBridgingRelease(slot->function);
        slot->function = NULL;
    }
    if (slot->library) {
        (void)CFBridgingRelease(slot->library);
        slot->library = NULL;
    }
    free(slot->entry_point);
    slot->entry_point = NULL;
    slot->occupied = false;
}

/* Release every MTLBuffer copy held by a buffer slot and clear it. */
static void ne_buffer_slot_release(NEBufferSlot *slot) {
    for (uint32_t i = 0; i < NE_MTL_MAX_FRAMES_IN_FLIGHT; i++) {
        if (slot->copies[i]) {
            (void)CFBridgingRelease(slot->copies[i]);
            slot->copies[i] = NULL;
        }
    }
    slot->occupied = false;
}

/* ── Pool-aware lookup helpers ──────────────────────────────────────────── */

static id<MTLFunction> ne_shader_get_function(const NERenderer *renderer, NEShaderHandle handle) {
    if (!ne_shader_handle_valid(handle)) return nil;
    uint32_t index = handle.id - 1;
    if (index >= renderer->shader_cap || !renderer->shaders[index].occupied) return nil;
    return (__bridge id<MTLFunction>)renderer->shaders[index].function;
}

/*
 * Resolve a buffer handle to the MTLBuffer copy for a given frame.
 * Static buffers have a single copy (frame_index is ignored); dynamic buffers
 * select copy[frame_index % copy_count]. Pass frame_index 0 outside a frame.
 */
static id<MTLBuffer> ne_buffer_get_for_frame(const NERenderer *renderer, NEBufferHandle handle,
                                             uint32_t frame_index) {
    if (!ne_buffer_handle_valid(handle)) return nil;
    uint32_t index = handle.id - 1;
    if (index >= renderer->buffer_cap || !renderer->buffers[index].occupied) return nil;
    const NEBufferSlot *slot = &renderer->buffers[index];
    uint32_t copy = frame_index % ne_buffer_slot_copy_count(slot);
    return (__bridge id<MTLBuffer>)slot->copies[copy];
}

static id<MTLDevice> ne_renderer_get_device(const NERenderer *renderer) {
    return renderer && renderer->device ? (__bridge id<MTLDevice>)renderer->device : nil;
}

static id<MTLCommandQueue> ne_renderer_get_queue(const NERenderer *renderer) {
    return renderer && renderer->queue ? (__bridge id<MTLCommandQueue>)renderer->queue : nil;
}

NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc) {
    (void)app;

    /*
     * enable_validation is a no-op for the Metal backend.
     * Metal's GPU validation is controlled via Xcode scheme settings or the
     * MTL_DEBUG_LAYER / MTL_SHADER_VALIDATION environment variables, not
     * programmatically at device creation time.
     */
    (void)desc;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        NE_LOG_ERROR("failed to create Metal device");
        return NULL;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
        NE_LOG_ERROR("failed to create Metal command queue");
        return NULL;
    }

    NERenderer *renderer = (NERenderer *)calloc(1, sizeof(NERenderer));
    if (!renderer) {
        return NULL;
    }

    renderer->device = (__bridge_retained void *)device;
    renderer->queue = (__bridge_retained void *)queue;

    return renderer;
}

void ne_renderer_destroy(NERenderer *renderer) {
    if (!renderer) {
        return;
    }

    /* Release all live shaders. */
    for (uint32_t i = 0; i < renderer->shader_cap; i++) {
        if (renderer->shaders[i].occupied) {
            ne_shader_slot_release(&renderer->shaders[i]);
        }
    }
    free(renderer->shaders);
    renderer->shaders = NULL;
    renderer->shader_count = 0;
    renderer->shader_cap = 0;

    /* Release all live buffers. */
    for (uint32_t i = 0; i < renderer->buffer_cap; i++) {
        if (renderer->buffers[i].occupied) {
            ne_buffer_slot_release(&renderer->buffers[i]);
        }
    }
    free(renderer->buffers);
    renderer->buffers = NULL;
    renderer->buffer_count = 0;
    renderer->buffer_cap = 0;

    /* Release all live pipelines. */
    for (uint32_t i = 0; i < renderer->pipeline_cap; i++) {
        if (renderer->pipelines[i].occupied && renderer->pipelines[i].pipeline_state) {
            (void)CFBridgingRelease(renderer->pipelines[i].pipeline_state);
        }
    }
    free(renderer->pipelines);
    renderer->pipelines = NULL;
    renderer->pipeline_count = 0;
    renderer->pipeline_cap = 0;

    if (renderer->queue) {
        (void)CFBridgingRelease(renderer->queue);
        renderer->queue = NULL;
    }

    if (renderer->device) {
        (void)CFBridgingRelease(renderer->device);
        renderer->device = NULL;
    }

    free(renderer);
}

NERenderSurface *ne_renderer_create_surface(NERenderer *renderer, NEWindow *window, const NERenderSurfaceDesc *desc) {
    if (!renderer || !window) {
        return NULL;
    }

    if (!ne_window_is_open(window)) {
        return NULL;
    }

    NSView *view = (__bridge NSView *)ne_window_get_native_handle(window, NE_NATIVE_HANDLE_COCOA_NS_VIEW);
    if (!view) {
        return NULL;
    }

    /* Enforce one surface per window: a view has exactly one backing CAMetalLayer. */
    if (objc_getAssociatedObject(view, g_surface_assoc_key) != nil) {
        NE_LOG_ERROR("window already has a render surface");
        return NULL;
    }

    id<MTLDevice> device = ne_renderer_get_device(renderer);
    if (!device) {
        return NULL;
    }

    CAMetalLayer *layer = [CAMetalLayer layer];
    if (!layer) {
        return NULL;
    }

    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;

    /* Best-effort vsync configuration; presentation is still synced by default. */
    if (desc) {
        if ([layer respondsToSelector:@selector(setDisplaySyncEnabled:)]) {
            layer.displaySyncEnabled = desc->vsync ? YES : NO;
        }
    }

    /* Attach layer to the view. */
    [view setWantsLayer:YES];
    view.layer = layer;

    NERenderSurface *surface = (NERenderSurface *)calloc(1, sizeof(NERenderSurface));
    if (!surface) {
        return NULL;
    }

    surface->renderer = renderer;
    surface->window = window;
    surface->layer = (__bridge_retained void *)layer;

    /* Default clear color. */
    surface->clear_color[0] = 0.1f;
    surface->clear_color[1] = 0.1f;
    surface->clear_color[2] = 0.2f;
    surface->clear_color[3] = 1.0f;

    if (desc) {
        memcpy(surface->clear_color, desc->clear_color_rgba, sizeof(surface->clear_color));
    }

    surface->frame_semaphore = dispatch_semaphore_create(NE_MTL_MAX_FRAMES_IN_FLIGHT);

    objc_setAssociatedObject(view, g_surface_assoc_key, [NSValue valueWithPointer:surface], OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    return surface;
}

void ne_renderer_destroy_surface(NERenderer *renderer, NERenderSurface *surface) {
    (void)renderer;

    if (!surface) {
        return;
    }

    /* Clear association (if the window/view is still alive). */
    if (surface->window && ne_window_is_open(surface->window)) {
        NSView *view = (__bridge NSView *)ne_window_get_native_handle(surface->window, NE_NATIVE_HANDLE_COCOA_NS_VIEW);
        if (view) {
            id obj = objc_getAssociatedObject(view, g_surface_assoc_key);
            if ([obj isKindOfClass:[NSValue class]] && [(NSValue *)obj pointerValue] == surface) {
                objc_setAssociatedObject(view, g_surface_assoc_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            }
        }
    }

    if (surface->encoder) {
        (void)CFBridgingRelease(surface->encoder);
        surface->encoder = NULL;
    }
    if (surface->command_buffer) {
        (void)CFBridgingRelease(surface->command_buffer);
        surface->command_buffer = NULL;
    }
    if (surface->drawable) {
        (void)CFBridgingRelease(surface->drawable);
        surface->drawable = NULL;
    }
    if (surface->layer) {
        (void)CFBridgingRelease(surface->layer);
        surface->layer = NULL;
    }

    surface->window = NULL;
    surface->renderer = NULL;

    free(surface);
}

void ne_renderer_surface_set_clear_color(NERenderSurface *surface, float r, float g, float b, float a) {
    if (!surface) {
        return;
    }
    surface->clear_color[0] = r;
    surface->clear_color[1] = g;
    surface->clear_color[2] = b;
    surface->clear_color[3] = a;
}

NERenderPass *ne_renderer_begin_frame(NERenderer *renderer, NERenderSurface *surface) {
    if (!renderer || !surface || surface->renderer != renderer) {
        return NULL;
    }

    if (!surface->window || !ne_window_is_open(surface->window)) {
        return NULL;
    }

    if (g_active_pass.surface) {
        NE_LOG_WARN("begin_frame called while a frame is already in progress — call ne_renderer_end_frame first");
        return NULL;
    }

    if (surface->drawable || surface->command_buffer) {
        NE_LOG_WARN("begin_frame called with dangling frame state on surface");
        return NULL;
    }

    /* Block until a frame slot is available (at most NE_MTL_MAX_FRAMES_IN_FLIGHT in flight). */
    dispatch_semaphore_wait(surface->frame_semaphore, DISPATCH_TIME_FOREVER);

    /*
     * From here on, every failure path must release the frame slot we just
     * acquired (NE_MTL_FRAME_ABORT) — otherwise the semaphore count leaks and,
     * after NE_MTL_MAX_FRAMES_IN_FLIGHT such failures, begin_frame deadlocks.
     * A nil `nextDrawable` is a normal occurrence during resize/occlusion, so
     * this is a hot path, not a rare one.
     *
     * (ARC forbids `goto` past __strong locals, so this is a macro rather than a
     * shared cleanup label.)
     */
#define NE_MTL_FRAME_ABORT()                                  \
    do {                                                      \
        dispatch_semaphore_signal(surface->frame_semaphore);  \
        return NULL;                                          \
    } while (0)

    CAMetalLayer *layer = ne_surface_get_layer(surface);
    if (!layer) {
        NE_MTL_FRAME_ABORT();
    }

    int32_t fb_w = 0;
    int32_t fb_h = 0;
    if (!ne_window_get_framebuffer_size(surface->window, &fb_w, &fb_h)) {
        NE_MTL_FRAME_ABORT();
    }
    if (fb_w <= 0 || fb_h <= 0) {
        NE_MTL_FRAME_ABORT();
    }

    layer.drawableSize = CGSizeMake((CGFloat)fb_w, (CGFloat)fb_h);

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        NE_MTL_FRAME_ABORT();
    }

    id<MTLCommandQueue> queue = ne_renderer_get_queue(renderer);
    if (!queue) {
        NE_MTL_FRAME_ABORT();
    }

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    if (!command_buffer) {
        NE_MTL_FRAME_ABORT();
    }

    MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
    rpd.colorAttachments[0].texture = drawable.texture;
    rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
    rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
    rpd.colorAttachments[0].clearColor = MTLClearColorMake(surface->clear_color[0], surface->clear_color[1], surface->clear_color[2],
                                                           surface->clear_color[3]);

    id<MTLRenderCommandEncoder> enc = [command_buffer renderCommandEncoderWithDescriptor:rpd];
    if (!enc) {
        NE_MTL_FRAME_ABORT();
    }

    /* Set a default viewport matching the framebuffer. */
    MTLViewport vp = { 0.0, 0.0, (double)fb_w, (double)fb_h, 0.0, 1.0 };
    [enc setViewport:vp];

    MTLScissorRect scissor = { 0, 0, (NSUInteger)fb_w, (NSUInteger)fb_h };
    [enc setScissorRect:scissor];

    surface->drawable = (__bridge_retained void *)drawable;
    surface->command_buffer = (__bridge_retained void *)command_buffer;
    surface->encoder = (__bridge_retained void *)enc;

    /*
     * Advance this surface's frame ring index. The semaphore wait above already
     * bounded us to NE_MTL_MAX_FRAMES_IN_FLIGHT frames in flight, so the copy we
     * roll onto is one the GPU has finished with. Dynamic-buffer updates/binds
     * this frame target this index.
     */
    surface->frame_index = (surface->frame_index + 1u) % NE_MTL_MAX_FRAMES_IN_FLIGHT;

    /* Reset per-pass draw state. */
    surface->current_topology = MTLPrimitiveTypeTriangle;
    surface->current_index_buffer = NE_BUFFER_HANDLE_NULL;
    surface->current_index_type = MTLIndexTypeUInt16;

    g_active_pass.surface = surface;
    return &g_active_pass;

#undef NE_MTL_FRAME_ABORT
}

void ne_renderer_end_frame(NERenderer *renderer, NERenderSurface *surface) {
    if (!renderer || !surface || surface->renderer != renderer) {
        return;
    }

    id<CAMetalDrawable> drawable = ne_surface_get_drawable(surface);
    id<MTLCommandBuffer> command_buffer = ne_surface_get_command_buffer(surface);
    if (!drawable || !command_buffer) {
        return;
    }

    /* End the render command encoder that was left open by begin_frame. */
    if (surface->encoder) {
        id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)surface->encoder;
        [enc endEncoding];
        (void)CFBridgingRelease(surface->encoder);
        surface->encoder = NULL;
    }

    /* Signal the frame semaphore when the GPU finishes this command buffer. */
    dispatch_semaphore_t sem = surface->frame_semaphore;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull cb) {
        (void)cb;
        dispatch_semaphore_signal(sem);
    }];

    [command_buffer presentDrawable:drawable];
    [command_buffer commit];

    (void)CFBridgingRelease(surface->command_buffer);
    surface->command_buffer = NULL;

    (void)CFBridgingRelease(surface->drawable);
    surface->drawable = NULL;
}

/* ── Shaders ────────────────────────────────────────────────────────────── */

/**
 * Common helper: given an MTLLibrary and an entry point name, resolves the
 * MTLFunction, allocates a pool slot, and returns the handle.
 */
static NEShaderHandle ne_shader_finish_create(NERenderer *renderer, NEShaderStage stage,
                                              id<MTLLibrary> library, const char *entry_point) {
    NSString *ep = [NSString stringWithUTF8String:entry_point];
    id<MTLFunction> function = [library newFunctionWithName:ep];
    if (!function) {
        NE_LOG_ERROR("shader entry point '%s' not found in library", entry_point);
        return NE_SHADER_HANDLE_NULL;
    }

    uint32_t index = ne_pool_alloc((void **)&renderer->shaders, &renderer->shader_cap, sizeof(NEShaderSlot));
    if (index == UINT32_MAX) {
        NE_LOG_ERROR("shader pool allocation failed");
        return NE_SHADER_HANDLE_NULL;
    }

    NEShaderSlot *slot = &renderer->shaders[index];
    slot->occupied = true;
    slot->stage = stage;
    slot->library = (__bridge_retained void *)library;
    slot->function = (__bridge_retained void *)function;
    slot->entry_point = strdup(entry_point);
    renderer->shader_count++;

    return (NEShaderHandle){ .id = index + 1 };
}

/**
 * Create a shader from pre-compiled bytecode (metallib).
 *
 * NOTE: Each call creates its own MTLLibrary even if the same bytecode blob is
 *       passed multiple times (e.g. vertex + fragment from the same metallib).
 *       A library cache keyed on the data pointer/size could deduplicate this
 *       in the future if it becomes a bottleneck.
 */
NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc) {
    if (!renderer || !desc || !desc->bytecode || desc->bytecode_size == 0 || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }

    id<MTLDevice> device = ne_renderer_get_device(renderer);
    if (!device) {
        return NE_SHADER_HANDLE_NULL;
    }

    dispatch_data_t data = dispatch_data_create(desc->bytecode, desc->bytecode_size,
                                                NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (!data) {
        return NE_SHADER_HANDLE_NULL;
    }

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    if (!library) {
        NE_LOG_ERROR("failed to create Metal library from bytecode: %s",
                     error ? [[error localizedDescription] UTF8String] : "unknown error");
        return NE_SHADER_HANDLE_NULL;
    }

    return ne_shader_finish_create(renderer, desc->stage, library, desc->entry_point);
}

/**
 * Create a shader by compiling MSL source at runtime.
 *
 * On the Metal backend the source is expected to be Metal Shading Language.
 * A future Slang integration path would first transpile Slang → MSL and then
 * feed the result into this same compilation path.
 */
void ne_renderer_set_shader_optimization(NERenderer *renderer, NEShaderOptimization level) {
    if (!renderer) {
        return;
    }
    renderer->shader_optimization = level;
}

NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc) {
    if (!renderer || !desc || !desc->source || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }

    id<MTLDevice> device = ne_renderer_get_device(renderer);
    if (!device) {
        return NE_SHADER_HANDLE_NULL;
    }

    NSString *source = [NSString stringWithUTF8String:desc->source];
    MTLCompileOptions *options = [[MTLCompileOptions alloc] init];

    /*
     * Metal does not expose a general optimisation level API.
     * The closest equivalent is controlling fast-math: enabled for PERFORMANCE,
     * disabled otherwise (preserves strict IEEE 754 compliance).
     *
     * mathMode was introduced in macOS 15 / iOS 18 to replace the deprecated
     * fastMathEnabled property.  We use it when available and fall back to the
     * legacy property on older SDKs.
     */
    const BOOL fast_math = (renderer->shader_optimization == NE_SHADER_OPTIMIZATION_PERFORMANCE)
                           ? YES : NO;
#if defined(__MAC_15_0) || defined(__IPHONE_18_0)
    if (@available(macOS 15.0, iOS 18.0, *)) {
        options.mathMode = fast_math ? MTLMathModeFast : MTLMathModeSafe;
    } else {
        /* Suppress deprecation warning: intentional fallback for older OS. */
        _Pragma("clang diagnostic push")
        _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
        options.fastMathEnabled = fast_math;
        _Pragma("clang diagnostic pop")
    }
#else
    options.fastMathEnabled = fast_math;
#endif

    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (!library) {
        NE_LOG_ERROR("failed to compile MSL source%s%s: %s",
                     desc->filename ? " (" : "",
                     desc->filename ? desc->filename : "",
                     error ? [[error localizedDescription] UTF8String] : "unknown error");
        return NE_SHADER_HANDLE_NULL;
    }

    return ne_shader_finish_create(renderer, desc->stage, library, desc->entry_point);
}

void ne_shader_destroy(NERenderer *renderer, NEShaderHandle handle) {
    if (!renderer || !ne_shader_handle_valid(handle)) {
        return;
    }

    uint32_t index = handle.id - 1;
    if (index >= renderer->shader_cap || !renderer->shaders[index].occupied) {
        NE_LOG_WARN("attempted to destroy invalid shader handle (id=%u)", handle.id);
        return;
    }

    ne_shader_slot_release(&renderer->shaders[index]);
    renderer->shader_count--;
}

/* ── Buffers ────────────────────────────────────────────────────────────── */

NEBufferHandle ne_buffer_create(NERenderer *renderer, const NEBufferDesc *desc) {
    if (!renderer || !desc || desc->size == 0) {
        return NE_BUFFER_HANDLE_NULL;
    }

    if (!desc->usage) {
        NE_LOG_ERROR("buffer creation requires at least one usage flag");
        return NE_BUFFER_HANDLE_NULL;
    }

    id<MTLDevice> device = ne_renderer_get_device(renderer);
    if (!device) {
        return NE_BUFFER_HANDLE_NULL;
    }

    /*
     * StorageModeShared (CPU/GPU-coherent, no separate VRAM copy) is both
     * correct and optimal on Apple Silicon's unified memory, which is our only
     * macOS target. Updates are therefore a plain memcpy into the buffer's
     * contents — no staging buffer or blit. (Discrete-GPU StorageModePrivate +
     * staging is intentionally not supported.)
     *
     * Dynamic buffers allocate one copy per in-flight frame so the CPU can
     * write next frame's copy while the GPU still reads the current one — no
     * stall, no race. The copy is selected per frame by the owning surface's
     * frame_index (see ne_render_pass_update_buffer / ne_buffer_get_for_frame).
     * Static buffers allocate a single shared copy.
     */
    uint32_t index = ne_pool_alloc((void **)&renderer->buffers, &renderer->buffer_cap, sizeof(NEBufferSlot));
    if (index == UINT32_MAX) {
        NE_LOG_ERROR("buffer pool allocation failed");
        return NE_BUFFER_HANDLE_NULL;
    }

    NEBufferSlot *slot = &renderer->buffers[index];
    slot->occupied = true;
    slot->dynamic = desc->dynamic;
    slot->usage = desc->usage;
    slot->size = desc->size;

    const uint32_t copy_count = ne_buffer_slot_copy_count(slot);
    for (uint32_t i = 0; i < copy_count; i++) {
        id<MTLBuffer> buffer = nil;
        if (desc->initial_data) {
            buffer = [device newBufferWithBytes:desc->initial_data
                                         length:desc->size
                                        options:MTLResourceStorageModeShared];
        } else {
            buffer = [device newBufferWithLength:desc->size
                                         options:MTLResourceStorageModeShared];
        }

        if (!buffer) {
            NE_LOG_ERROR("failed to create Metal buffer (%u bytes)", desc->size);
            ne_buffer_slot_release(slot); /* releases copies created so far */
            return NE_BUFFER_HANDLE_NULL;
        }
        slot->copies[i] = (__bridge_retained void *)buffer;
    }

    renderer->buffer_count++;

    return (NEBufferHandle){ .id = index + 1 };
}

/* Validate a buffer slot for an update and bounds-check the region.
 * Returns the slot, or NULL (with a log) on any failure. */
static NEBufferSlot *ne_buffer_update_validate(NERenderer *renderer, NEBufferHandle handle,
                                               const void *data, uint32_t size, uint32_t offset) {
    if (!renderer || !ne_buffer_handle_valid(handle) || !data || size == 0) {
        return NULL;
    }

    uint32_t index = handle.id - 1;
    if (index >= renderer->buffer_cap || !renderer->buffers[index].occupied) {
        NE_LOG_WARN("attempted to update invalid buffer handle (id=%u)", handle.id);
        return NULL;
    }

    NEBufferSlot *slot = &renderer->buffers[index];
    /* Overflow-safe bounds check: `offset + size` could wrap around uint32_t. */
    if (size > slot->size || offset > slot->size - size) {
        NE_LOG_ERROR("buffer update out of bounds (offset=%u + size=%u > buffer_size=%u)",
                     offset, size, slot->size);
        return NULL;
    }
    return slot;
}

static void ne_buffer_copy_write(NEBufferSlot *slot, uint32_t copy,
                                 const void *data, uint32_t size, uint32_t offset) {
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)slot->copies[copy];
    memcpy((uint8_t *)[buffer contents] + offset, data, size);
}

void ne_buffer_update(NERenderer *renderer, NEBufferHandle handle,
                      const void *data, uint32_t size, uint32_t offset) {
    NEBufferSlot *slot = ne_buffer_update_validate(renderer, handle, data, size, offset);
    if (!slot) {
        return;
    }

    /*
     * Renderer-scoped update: intended for static/one-time setup (no frame
     * context). For a dynamic buffer we have no surface frame_index here, so we
     * write ALL copies to keep them consistent. This is only safe outside an
     * in-flight frame (e.g. before the first frame). Per-frame dynamic updates
     * must use ne_render_pass_update_buffer instead.
     */
    if (slot->dynamic) {
        NE_LOG_WARN("ne_buffer_update on a dynamic buffer writes all copies; "
                    "use ne_render_pass_update_buffer for per-frame updates");
    }
    const uint32_t copy_count = ne_buffer_slot_copy_count(slot);
    for (uint32_t i = 0; i < copy_count; i++) {
        ne_buffer_copy_write(slot, i, data, size, offset);
    }
}

void ne_buffer_destroy(NERenderer *renderer, NEBufferHandle handle) {
    if (!renderer || !ne_buffer_handle_valid(handle)) {
        return;
    }

    uint32_t index = handle.id - 1;
    if (index >= renderer->buffer_cap || !renderer->buffers[index].occupied) {
        NE_LOG_WARN("attempted to destroy invalid buffer handle (id=%u)", handle.id);
        return;
    }

    ne_buffer_slot_release(&renderer->buffers[index]);
    renderer->buffer_count--;
}

/* ── Pipelines ──────────────────────────────────────────────────────────── */

static MTLVertexFormat ne_vertex_format_to_mtl(NEVertexFormat fmt) {
    switch (fmt) {
    case NE_VERTEX_FORMAT_FLOAT:    return MTLVertexFormatFloat;
    case NE_VERTEX_FORMAT_FLOAT2:   return MTLVertexFormatFloat2;
    case NE_VERTEX_FORMAT_FLOAT3:   return MTLVertexFormatFloat3;
    case NE_VERTEX_FORMAT_FLOAT4:   return MTLVertexFormatFloat4;
    case NE_VERTEX_FORMAT_UNORM8X4: return MTLVertexFormatUChar4Normalized;
    default:                        return MTLVertexFormatFloat4;
    }
}

static MTLPrimitiveType ne_topology_to_mtl(NEPrimitiveTopology t) {
    switch (t) {
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return MTLPrimitiveTypeTriangle;
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    case NE_PRIMITIVE_TOPOLOGY_LINE_LIST:      return MTLPrimitiveTypeLine;
    case NE_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
    case NE_PRIMITIVE_TOPOLOGY_POINT_LIST:     return MTLPrimitiveTypePoint;
    default:                                   return MTLPrimitiveTypeTriangle;
    }
}

static MTLBlendFactor ne_blend_factor_to_mtl(NEBlendFactor f) {
    switch (f) {
    case NE_BLEND_FACTOR_ZERO:                return MTLBlendFactorZero;
    case NE_BLEND_FACTOR_ONE:                 return MTLBlendFactorOne;
    case NE_BLEND_FACTOR_SRC_ALPHA:           return MTLBlendFactorSourceAlpha;
    case NE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case NE_BLEND_FACTOR_DST_ALPHA:           return MTLBlendFactorDestinationAlpha;
    case NE_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    default:                                  return MTLBlendFactorOne;
    }
}

static MTLBlendOperation ne_blend_op_to_mtl(NEBlendOp op) {
    switch (op) {
    case NE_BLEND_OP_ADD:              return MTLBlendOperationAdd;
    case NE_BLEND_OP_SUBTRACT:         return MTLBlendOperationSubtract;
    case NE_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case NE_BLEND_OP_MIN:              return MTLBlendOperationMin;
    case NE_BLEND_OP_MAX:              return MTLBlendOperationMax;
    default:                           return MTLBlendOperationAdd;
    }
}

NEPipelineHandle ne_pipeline_create(NERenderer *renderer, const NEPipelineDesc *desc) {
    if (!renderer || !desc) {
        return NE_PIPELINE_HANDLE_NULL;
    }

    id<MTLFunction> vertex_fn   = ne_shader_get_function(renderer, desc->vertex_shader);
    id<MTLFunction> fragment_fn = ne_shader_get_function(renderer, desc->fragment_shader);
    if (!vertex_fn || !fragment_fn) {
        NE_LOG_ERROR("pipeline creation failed: invalid vertex or fragment shader");
        return NE_PIPELINE_HANDLE_NULL;
    }

    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vertex_fn;
    pd.fragmentFunction = fragment_fn;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

    /* Blend state. */
    if (desc->blend.enabled) {
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor        = ne_blend_factor_to_mtl(desc->blend.src_color);
        pd.colorAttachments[0].destinationRGBBlendFactor    = ne_blend_factor_to_mtl(desc->blend.dst_color);
        pd.colorAttachments[0].rgbBlendOperation            = ne_blend_op_to_mtl(desc->blend.color_op);
        pd.colorAttachments[0].sourceAlphaBlendFactor       = ne_blend_factor_to_mtl(desc->blend.src_alpha);
        pd.colorAttachments[0].destinationAlphaBlendFactor  = ne_blend_factor_to_mtl(desc->blend.dst_alpha);
        pd.colorAttachments[0].alphaBlendOperation          = ne_blend_op_to_mtl(desc->blend.alpha_op);
    }

    /* Vertex descriptor from NEVertexBufferLayout(s). */
    if (desc->vertex_layouts && desc->vertex_layout_count > 0) {
        MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];

        for (uint32_t buf = 0; buf < desc->vertex_layout_count; buf++) {
            const NEVertexBufferLayout *layout = &desc->vertex_layouts[buf];
            vd.layouts[buf].stride = layout->stride;
            vd.layouts[buf].stepFunction = MTLVertexStepFunctionPerVertex;

            for (uint32_t a = 0; a < layout->attribute_count; a++) {
                const NEVertexAttribute *attr = &layout->attributes[a];
                vd.attributes[attr->location].format      = ne_vertex_format_to_mtl(attr->format);
                vd.attributes[attr->location].offset      = attr->offset;
                vd.attributes[attr->location].bufferIndex = buf;
            }
        }

        pd.vertexDescriptor = vd;
    }

    id<MTLDevice> device = ne_renderer_get_device(renderer);
    NSError *error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pso) {
        NE_LOG_ERROR("failed to create Metal pipeline state: %s",
                     error ? [[error localizedDescription] UTF8String] : "unknown error");
        return NE_PIPELINE_HANDLE_NULL;
    }

    uint32_t index = ne_pool_alloc((void **)&renderer->pipelines, &renderer->pipeline_cap, sizeof(NEPipelineSlot));
    if (index == UINT32_MAX) {
        NE_LOG_ERROR("pipeline pool allocation failed");
        return NE_PIPELINE_HANDLE_NULL;
    }

    NEPipelineSlot *slot = &renderer->pipelines[index];
    slot->occupied = true;
    slot->pipeline_state = (__bridge_retained void *)pso;
    slot->topology = (uint32_t)ne_topology_to_mtl(desc->topology);
    renderer->pipeline_count++;

    return (NEPipelineHandle){ .id = index + 1 };
}

void ne_pipeline_destroy(NERenderer *renderer, NEPipelineHandle handle) {
    if (!renderer || !ne_pipeline_handle_valid(handle)) {
        return;
    }

    uint32_t index = handle.id - 1;
    if (index >= renderer->pipeline_cap || !renderer->pipelines[index].occupied) {
        NE_LOG_WARN("attempted to destroy invalid pipeline handle (id=%u)", handle.id);
        return;
    }

    NEPipelineSlot *slot = &renderer->pipelines[index];
    if (slot->pipeline_state) {
        (void)CFBridgingRelease(slot->pipeline_state);
        slot->pipeline_state = NULL;
    }
    slot->occupied = false;
    renderer->pipeline_count--;
}

NEComputePipelineHandle ne_compute_pipeline_create(NERenderer *renderer, const NEComputePipelineDesc *desc) {
    (void)renderer; (void)desc;
    NE_LOG_WARN("compute pipelines not yet implemented on Metal");
    return NE_COMPUTE_PIPELINE_HANDLE_NULL;
}

void ne_compute_pipeline_destroy(NERenderer *renderer, NEComputePipelineHandle handle) {
    (void)renderer; (void)handle;
}

/* ── Render pass commands ───────────────────────────────────────────────── */

static id<MTLRenderCommandEncoder> ne_pass_get_encoder(NERenderPass *pass) {
    if (!pass || !pass->surface || !pass->surface->encoder) return nil;
    return (__bridge id<MTLRenderCommandEncoder>)pass->surface->encoder;
}

void ne_render_pass_set_pipeline(NERenderPass *pass, NEPipelineHandle pipeline) {
    id<MTLRenderCommandEncoder> enc = ne_pass_get_encoder(pass);
    if (!enc) return;

    NERenderer *renderer = pass->surface->renderer;
    uint32_t index = pipeline.id - 1;
    if (index >= renderer->pipeline_cap || !renderer->pipelines[index].occupied) {
        NE_LOG_WARN("set_pipeline: invalid pipeline handle (id=%u)", pipeline.id);
        return;
    }

    NEPipelineSlot *slot = &renderer->pipelines[index];
    id<MTLRenderPipelineState> pso = (__bridge id<MTLRenderPipelineState>)slot->pipeline_state;
    [enc setRenderPipelineState:pso];
    pass->surface->current_topology = slot->topology;
}

void ne_render_pass_set_vertex_buffer(NERenderPass *pass, uint64_t slot, NEBufferHandle buffer) {
    id<MTLRenderCommandEncoder> enc = ne_pass_get_encoder(pass);
    if (!enc) return;

    id<MTLBuffer> mtl_buf = ne_buffer_get_for_frame(pass->surface->renderer, buffer,
                                                    pass->surface->frame_index);
    if (!mtl_buf) {
        NE_LOG_WARN("set_vertex_buffer: invalid buffer handle (id=%u)", buffer.id);
        return;
    }

    [enc setVertexBuffer:mtl_buf offset:0 atIndex:slot];
}

void ne_render_pass_set_index_buffer(NERenderPass *pass, NEBufferHandle buffer, NEIndexType type) {
    if (!pass || !pass->surface) return;

    if (!ne_buffer_get_for_frame(pass->surface->renderer, buffer, pass->surface->frame_index)) {
        NE_LOG_WARN("set_index_buffer: invalid buffer handle (id=%u)", buffer.id);
        return;
    }

    /* Store for use at draw_indexed time. Borrowed pointer — not retained. */
    pass->surface->current_index_buffer = buffer;
    pass->surface->current_index_type = (type == NE_INDEX_TYPE_UINT32)
                                            ? (uint32_t)MTLIndexTypeUInt32
                                            : (uint32_t)MTLIndexTypeUInt16;
}

void ne_render_pass_set_uniform_data(NERenderPass *pass, NEShaderStage stage, uint64_t slot, const void *data, size_t size) {
    id<MTLRenderCommandEncoder> enc = ne_pass_get_encoder(pass);
    if (!enc || !data || size == 0) return;

    switch (stage) {
    case NE_SHADER_STAGE_VERTEX:
        [enc setVertexBytes:data length:size atIndex:slot];
        break;
    case NE_SHADER_STAGE_FRAGMENT:
        [enc setFragmentBytes:data length:size atIndex:slot];
        break;
    default:
        NE_LOG_WARN("set_uniform_data: unsupported stage for render pass");
        break;
    }
}

void ne_render_pass_update_buffer(NERenderPass *pass, NEBufferHandle handle,
                                  const void *data, uint32_t size, uint32_t offset) {
    if (!pass || !pass->surface) return;

    NERenderer *renderer = pass->surface->renderer;
    NEBufferSlot *slot = ne_buffer_update_validate(renderer, handle, data, size, offset);
    if (!slot) {
        return;
    }

    if (!slot->dynamic) {
        NE_LOG_WARN("ne_render_pass_update_buffer: buffer (id=%u) is not dynamic; "
                    "create it with NEBufferDesc.dynamic = true", handle.id);
        return;
    }

    /*
     * Write only the copy bound for this frame. The semaphore-bounded ring
     * guarantees the GPU is no longer reading this copy, so the write neither
     * stalls nor races. A dynamic buffer must be updated every frame before it
     * is drawn — the other copies hold prior frames' data.
     */
    uint32_t copy = pass->surface->frame_index % ne_buffer_slot_copy_count(slot);
    ne_buffer_copy_write(slot, copy, data, size, offset);
}

void ne_render_pass_draw(NERenderPass *pass, uint64_t first_vertex, uint64_t vertex_count) {
    id<MTLRenderCommandEncoder> enc = ne_pass_get_encoder(pass);
    if (!enc) return;

    [enc drawPrimitives:(MTLPrimitiveType)pass->surface->current_topology
            vertexStart:first_vertex
            vertexCount:vertex_count];
}

void ne_render_pass_draw_indexed(NERenderPass *pass, uint64_t index_count,
                                 uint64_t first_index, int64_t vertex_offset) {
    id<MTLRenderCommandEncoder> enc = ne_pass_get_encoder(pass);
    if (!enc) return;

    NERenderSurface *surface = pass->surface;
    if (!surface->current_index_buffer.id) {
        NE_LOG_WARN("draw_indexed called without a bound index buffer");
        return;
    }

    id<MTLBuffer> idx_buf = ne_buffer_get_for_frame(pass->surface->renderer,
                                                    surface->current_index_buffer,
                                                    surface->frame_index);
    MTLIndexType idx_type = (MTLIndexType)surface->current_index_type;
    NSUInteger idx_size = (idx_type == MTLIndexTypeUInt32) ? 4 : 2;

    [enc drawIndexedPrimitives:(MTLPrimitiveType)surface->current_topology
                    indexCount:index_count
                     indexType:idx_type
                   indexBuffer:idx_buf
             indexBufferOffset:first_index * idx_size
                 instanceCount:1
                    baseVertex:vertex_offset
                  baseInstance:0];
}

/* ── Compute pass stubs ─────────────────────────────────────────────────── */

NEComputePass *ne_render_pass_begin_compute(NERenderPass *pass) {
    (void)pass;
    NE_LOG_WARN("compute passes not yet implemented on Metal");
    return NULL;
}

void ne_render_pass_end_compute(NERenderPass *pass, NEComputePass *compute) {
    (void)pass; (void)compute;
}

void ne_compute_pass_set_pipeline(NEComputePass *pass, NEComputePipelineHandle pipeline) {
    (void)pass; (void)pipeline;
}

void ne_compute_pass_set_storage_buffer(NEComputePass *pass, uint64_t slot, NEBufferHandle buffer) {
    (void)pass; (void)slot; (void)buffer;
}

void ne_compute_pass_set_uniform_data(NEComputePass *pass, uint64_t slot,
                                      const void *data, uint32_t size) {
    (void)pass; (void)slot; (void)data; (void)size;
}

void ne_compute_pass_dispatch(NEComputePass *pass, uint64_t group_count_x,
                              uint64_t group_count_y, uint64_t group_count_z) {
    (void)pass; (void)group_count_x; (void)group_count_y; (void)group_count_z;
}
