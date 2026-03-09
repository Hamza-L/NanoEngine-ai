#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_shader.h"
#include "ne_window.h"

#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

enum {
    NE_MTL_MAX_FRAMES_IN_FLIGHT = 2,
    NE_MTL_SHADER_POOL_INITIAL_CAP = 16,
};

/* ── Shader pool ────────────────────────────────────────────────────────── */

typedef struct NEShaderSlot {
    bool occupied;
    NEShaderStage stage;
    void *library;     /* id<MTLLibrary>  */
    void *function;    /* id<MTLFunction> */
    char *entry_point; /* strdup'd copy   */
} NEShaderSlot;

/* ── Renderer ───────────────────────────────────────────────────────────── */

struct NERenderer {
    void *device;
    void *queue;

    NEShaderSlot *shaders;
    uint32_t shader_count;
    uint32_t shader_cap;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    void *layer; /* CAMetalLayer* */
    void *drawable; /* id<CAMetalDrawable> */
    void *command_buffer; /* id<MTLCommandBuffer> */

    float clear_color[4];

    /* Frame pacing: limits how far the CPU can get ahead of the GPU. */
    dispatch_semaphore_t frame_semaphore;
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

/**
 * Finds an available slot in the shader pool, growing it if necessary.
 * Returns the slot index, or UINT32_MAX on allocation failure.
 */
static uint32_t ne_shader_pool_alloc(NERenderer *renderer) {
    /* Scan for a free slot. */
    for (uint32_t i = 0; i < renderer->shader_cap; i++) {
        if (!renderer->shaders[i].occupied) {
            return i;
        }
    }

    /* Grow the pool. */
    uint32_t new_cap = renderer->shader_cap == 0
                           ? NE_MTL_SHADER_POOL_INITIAL_CAP
                           : renderer->shader_cap * 2;
    NEShaderSlot *new_slots = (NEShaderSlot *)realloc(renderer->shaders, new_cap * sizeof(NEShaderSlot));
    if (!new_slots) {
        return UINT32_MAX;
    }
    memset(new_slots + renderer->shader_cap, 0, (new_cap - renderer->shader_cap) * sizeof(NEShaderSlot));

    uint32_t index = renderer->shader_cap;
    renderer->shaders = new_slots;
    renderer->shader_cap = new_cap;
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

    CAMetalLayer *layer = ne_surface_get_layer(surface);
    if (!layer) {
        return NULL;
    }

    int32_t fb_w = 0;
    int32_t fb_h = 0;
    if (!ne_window_get_framebuffer_size(surface->window, &fb_w, &fb_h)) {
        return NULL;
    }
    if (fb_w <= 0 || fb_h <= 0) {
        return NULL;
    }

    layer.drawableSize = CGSizeMake((CGFloat)fb_w, (CGFloat)fb_h);

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        return NULL;
    }

    id<MTLCommandQueue> queue = ne_renderer_get_queue(renderer);
    if (!queue) {
        return NULL;
    }

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    if (!command_buffer) {
        return NULL;
    }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(surface->clear_color[0], surface->clear_color[1], surface->clear_color[2],
                                                           surface->clear_color[3]);

    id<MTLRenderCommandEncoder> enc = [command_buffer renderCommandEncoderWithDescriptor:pass];
    [enc endEncoding];

    surface->drawable = (__bridge_retained void *)drawable;
    surface->command_buffer = (__bridge_retained void *)command_buffer;

    g_active_pass.surface = surface;
    return &g_active_pass;
}

void ne_renderer_end_frame(NERenderer *renderer, NERenderPass *pass) {
    if (!renderer || !pass || !pass->surface) {
        return;
    }

    NERenderSurface *surface = pass->surface;
    if (surface->renderer != renderer) {
        return;
    }

    id<CAMetalDrawable> drawable = ne_surface_get_drawable(surface);
    id<MTLCommandBuffer> command_buffer = ne_surface_get_command_buffer(surface);
    if (!drawable || !command_buffer) {
        return;
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

    pass->surface = NULL;
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

    uint32_t index = ne_shader_pool_alloc(renderer);
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
