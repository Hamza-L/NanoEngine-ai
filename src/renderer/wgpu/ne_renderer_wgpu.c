/*
 * WebGPU renderer backend (Emscripten / browser target).
 *
 * Structurally mirrors the Metal backend (src/renderer/metal/ne_renderer_metal.m):
 * opaque NERenderer/NERenderSurface/NERenderPass, three resource pools via the
 * shared ne_pool_alloc, id = slot_index + 1 handles, a single static
 * g_active_pass, a per-surface frame_index ring for dynamic buffers, and
 * ne_*_to_wgpu enum mappers paralleling ne_*_to_mtl.
 *
 * IMPORTANT: this file targets Emscripten's WebGPU. It cannot be compiled in a
 * native build (no webgpu.h there) and is guarded accordingly. The WebGPU C ABI
 * has changed across emsdk versions — see the "API-version landmines" notes at
 * the call sites most likely to need adjustment.
 *
 * Frame driving / pacing is the browser's job (requestAnimationFrame, via the
 * web platform backend's emscripten_set_main_loop). Unlike Metal there is no
 * frame semaphore here, and unlike native there is no explicit present call:
 * the browser presents when control returns to it.
 */

#if !defined(__EMSCRIPTEN__)
#error "WebGPU renderer backend targets Emscripten (build with PLATFORM=web)"
#endif

#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_image.h"
#include "ne_renderer_pass.h"
#include "ne_renderer_pipeline.h"
#include "ne_renderer_shader.h"
#include "ne_window.h"
#include "ne_alloc.h"

#include <emscripten.h>
#include <webgpu/webgpu.h>

#include <stdlib.h>
#include <string.h>

enum {
    NE_WGPU_MAX_FRAMES_IN_FLIGHT = 3,
};

/* strdup is POSIX, not exposed under -std=c23 -Wpedantic; provide a local one. */
static char *ne_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* Build a Dawn WGPUStringView from a null-terminated C string. */
static WGPUStringView ne_sv(const char *s) {
    WGPUStringView v;
    v.data = s;
    v.length = s ? strlen(s) : 0;
    return v;
}

/* ── Resource pool slot types ───────────────────────────────────────────── */

typedef struct NEShaderSlot {
    bool occupied;
    NEShaderStage stage;
    WGPUShaderModule module; /* WGSL module; holds all entry points */
    char *entry_point;       /* strdup'd; consumed at pipeline-create time */
} NEShaderSlot;

typedef struct NEBufferSlot {
    bool occupied;
    bool dynamic; /* true: per-frame copies, safe to update while GPU reads another */
    uint32_t usage;
    uint32_t size;
    WGPUBuffer copies[NE_WGPU_MAX_FRAMES_IN_FLIGHT]; /* dynamic uses all, static uses [0] */
} NEBufferSlot;

typedef struct NEPipelineSlot {
    bool occupied;
    WGPURenderPipeline pipeline;
    WGPUPrimitiveTopology topology;
} NEPipelineSlot;

/* Copies a buffer slot holds: one per in-flight frame if dynamic, else one. */
static inline uint32_t ne_buffer_slot_copy_count(const NEBufferSlot *slot) {
    return slot->dynamic ? (uint32_t)NE_WGPU_MAX_FRAMES_IN_FLIGHT : 1u;
}

/* ── Renderer ───────────────────────────────────────────────────────────── */

struct NERenderer {
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;

    /*
     * Device acquisition is asynchronous on the web (RequestAdapter →
     * RequestDevice callbacks) and cannot be spin-waited — the browser owns the
     * loop. ne_renderer_create returns immediately with device_ready == false;
     * begin_frame and all resource-create functions gate on device_ready.
     */
    bool device_ready;
    bool device_failed;

    NEShaderSlot *shaders;
    uint32_t shader_count;
    uint32_t shader_cap;

    NEBufferSlot *buffers;
    uint32_t buffer_count;
    uint32_t buffer_cap;

    NEPipelineSlot *pipelines;
    uint32_t pipeline_count;
    uint32_t pipeline_cap;

    /* Chosen once a surface + device are ready (in begin_frame). */
    WGPUTextureFormat surface_format;

    NEShaderOptimization shader_optimization;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    WGPUSurface surface;
    char *canvas_selector; /* strdup'd, e.g. "#canvas" */

    bool configured;
    int32_t configured_w;
    int32_t configured_h;

    float clear_color[4];

    /*
     * Per-surface frame ring index (0 .. NE_WGPU_MAX_FRAMES_IN_FLIGHT-1),
     * advanced once per begin_frame. Selects which copy of a dynamic buffer to
     * write/bind this frame. The browser does not run more than a frame or two
     * ahead via rAF, so the ring is race-free without an explicit fence.
     */
    uint32_t frame_index;

    /* Current-frame transient state (valid between begin_frame / end_frame). */
    WGPUCommandEncoder encoder;
    WGPURenderPassEncoder pass_encoder;
    WGPUTextureView view;
    WGPUTexture texture;

    WGPUPrimitiveTopology current_topology;
    NEBufferHandle current_index_buffer;
    WGPUIndexFormat current_index_format;
};

/*
 * NERenderPass is a lightweight token pointing back to the owning surface. A
 * single static instance is reused each frame (one frame in flight at a time),
 * exactly like the Metal backend.
 */
struct NERenderPass {
    NERenderSurface *surface;
};

static NERenderPass g_active_pass = {0};

/* ── Async device acquisition ───────────────────────────────────────────── */

/*
 * Dawn / emdawnwebgpu callback ABI: callbacks are delivered through
 * WGPURequest*CallbackInfo structs, messages arrive as WGPUStringView (ptr+len),
 * and there are two userdata pointers. Status success is *_Success.
 */
static void ne_wgpu_on_device(WGPURequestDeviceStatus status, WGPUDevice device,
                              WGPUStringView message, void *userdata1, void *userdata2) {
    (void)userdata2;
    NERenderer *renderer = (NERenderer *)userdata1;
    if (status != WGPURequestDeviceStatus_Success || !device) {
        NE_LOG_ERROR("WebGPU device request failed: %.*s",
                     (int)message.length, message.data ? message.data : "");
        renderer->device_failed = true;
        return;
    }
    renderer->device = device;
    renderer->queue = wgpuDeviceGetQueue(device);
    renderer->device_ready = true;
    NE_LOG_INFO("WebGPU device ready");
}

static void ne_wgpu_on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                               WGPUStringView message, void *userdata1, void *userdata2) {
    (void)userdata2;
    NERenderer *renderer = (NERenderer *)userdata1;
    if (status != WGPURequestAdapterStatus_Success || !adapter) {
        NE_LOG_ERROR("WebGPU adapter request failed: %.*s",
                     (int)message.length, message.data ? message.data : "");
        renderer->device_failed = true;
        return;
    }
    renderer->adapter = adapter;

    WGPUDeviceDescriptor device_desc;
    memset(&device_desc, 0, sizeof(device_desc));

    WGPURequestDeviceCallbackInfo cb;
    memset(&cb, 0, sizeof(cb));
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = ne_wgpu_on_device;
    cb.userdata1 = renderer;
    wgpuAdapterRequestDevice(adapter, &device_desc, cb);
}

NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc) {
    (void)app;
    (void)desc;

    NERenderer *renderer = (NERenderer *)calloc(1, sizeof(NERenderer));
    if (!renderer) {
        return NULL;
    }

    renderer->instance = wgpuCreateInstance(NULL);
    if (!renderer->instance) {
        NE_LOG_ERROR("failed to create WebGPU instance");
        free(renderer);
        return NULL;
    }

    renderer->surface_format = WGPUTextureFormat_Undefined;

    /*
     * Kick off the async adapter → device chain. The renderer is returned
     * immediately in a pending state; callers must tolerate begin_frame /
     * resource creation returning failure until device_ready becomes true.
     */
    WGPURequestAdapterOptions adapter_opts;
    memset(&adapter_opts, 0, sizeof(adapter_opts));

    WGPURequestAdapterCallbackInfo cb;
    memset(&cb, 0, sizeof(cb));
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = ne_wgpu_on_adapter;
    cb.userdata1 = renderer;
    wgpuInstanceRequestAdapter(renderer->instance, &adapter_opts, cb);

    return renderer;
}

/* ── Slot release helpers ───────────────────────────────────────────────── */

static void ne_shader_slot_release(NEShaderSlot *slot) {
    if (slot->module) {
        wgpuShaderModuleRelease(slot->module);
        slot->module = NULL;
    }
    free(slot->entry_point);
    slot->entry_point = NULL;
    slot->occupied = false;
}

static void ne_buffer_slot_release(NEBufferSlot *slot) {
    for (uint32_t i = 0; i < NE_WGPU_MAX_FRAMES_IN_FLIGHT; i++) {
        if (slot->copies[i]) {
            wgpuBufferRelease(slot->copies[i]);
            slot->copies[i] = NULL;
        }
    }
    slot->occupied = false;
}

static void ne_pipeline_slot_release(NEPipelineSlot *slot) {
    if (slot->pipeline) {
        wgpuRenderPipelineRelease(slot->pipeline);
        slot->pipeline = NULL;
    }
    slot->occupied = false;
}

void ne_renderer_destroy(NERenderer *renderer) {
    if (!renderer) {
        return;
    }

    for (uint32_t i = 0; i < renderer->shader_cap; i++) {
        if (renderer->shaders[i].occupied) {
            ne_shader_slot_release(&renderer->shaders[i]);
        }
    }
    free(renderer->shaders);

    for (uint32_t i = 0; i < renderer->buffer_cap; i++) {
        if (renderer->buffers[i].occupied) {
            ne_buffer_slot_release(&renderer->buffers[i]);
        }
    }
    free(renderer->buffers);

    for (uint32_t i = 0; i < renderer->pipeline_cap; i++) {
        if (renderer->pipelines[i].occupied) {
            ne_pipeline_slot_release(&renderer->pipelines[i]);
        }
    }
    free(renderer->pipelines);

    /* Tolerate a renderer that never became ready (any of these may be NULL). */
    if (renderer->queue) {
        wgpuQueueRelease(renderer->queue);
    }
    if (renderer->device) {
        wgpuDeviceRelease(renderer->device);
    }
    if (renderer->adapter) {
        wgpuAdapterRelease(renderer->adapter);
    }
    if (renderer->instance) {
        wgpuInstanceRelease(renderer->instance);
    }

    free(renderer);
}

/* ── Surface ────────────────────────────────────────────────────────────── */

NERenderSurface *ne_renderer_create_surface(NERenderer *renderer, NEWindow *window, const NERenderSurfaceDesc *desc) {
    if (!renderer || !window) {
        return NULL;
    }
    if (!ne_window_is_open(window)) {
        return NULL;
    }

    /*
     * Default to the page's "#canvas". A web window backend may expose a
     * specific selector via ne_window_get_native_handle; surface creation does
     * NOT require the device, so it can run before device_ready.
     */
    const char *selector = "#canvas";

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_desc;
    memset(&canvas_desc, 0, sizeof(canvas_desc));
    canvas_desc.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas_desc.selector = ne_sv(selector);

    WGPUSurfaceDescriptor surface_desc;
    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.nextInChain = (WGPUChainedStruct *)&canvas_desc;

    WGPUSurface wgpu_surface = wgpuInstanceCreateSurface(renderer->instance, &surface_desc);
    if (!wgpu_surface) {
        NE_LOG_ERROR("failed to create WebGPU surface for selector '%s'", selector);
        return NULL;
    }

    NERenderSurface *surface = (NERenderSurface *)calloc(1, sizeof(NERenderSurface));
    if (!surface) {
        wgpuSurfaceRelease(wgpu_surface);
        return NULL;
    }

    surface->renderer = renderer;
    surface->window = window;
    surface->surface = wgpu_surface;
    surface->canvas_selector = ne_strdup(selector);

    /* Default clear color (matches the Metal backend). */
    surface->clear_color[0] = 0.1f;
    surface->clear_color[1] = 0.1f;
    surface->clear_color[2] = 0.2f;
    surface->clear_color[3] = 1.0f;
    if (desc) {
        memcpy(surface->clear_color, desc->clear_color_rgba, sizeof(surface->clear_color));
    }

    surface->current_index_format = WGPUIndexFormat_Uint16;

    return surface;
}

void ne_renderer_destroy_surface(NERenderer *renderer, NERenderSurface *surface) {
    (void)renderer;
    if (!surface) {
        return;
    }

    if (surface->pass_encoder) {
        wgpuRenderPassEncoderRelease(surface->pass_encoder);
    }
    if (surface->encoder) {
        wgpuCommandEncoderRelease(surface->encoder);
    }
    if (surface->view) {
        wgpuTextureViewRelease(surface->view);
    }
    if (surface->texture) {
        wgpuTextureRelease(surface->texture);
    }
    if (surface->surface) {
        wgpuSurfaceRelease(surface->surface);
    }
    free(surface->canvas_selector);
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

/* ── Frame lifecycle ────────────────────────────────────────────────────── */

NERenderPass *ne_renderer_begin_frame(NERenderer *renderer, NERenderSurface *surface) {
    if (!renderer || !surface || surface->renderer != renderer) {
        return NULL;
    }
    if (renderer->device_failed) {
        return NULL;
    }
    if (!renderer->device_ready) {
        return NULL; /* async device not ready yet — skip this frame */
    }
    if (!surface->window || !ne_window_is_open(surface->window)) {
        return NULL;
    }
    if (g_active_pass.surface) {
        NE_LOG_WARN("begin_frame called while a frame is already in progress — call ne_renderer_end_frame first");
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

    /* Choose the surface format once, now that adapter + surface both exist.
     * Dawn exposes the preferred format as capabilities.formats[0]. */
    if (renderer->surface_format == WGPUTextureFormat_Undefined) {
        WGPUSurfaceCapabilities caps;
        memset(&caps, 0, sizeof(caps));
        WGPUTextureFormat fmt = WGPUTextureFormat_BGRA8Unorm;
        if (wgpuSurfaceGetCapabilities(surface->surface, renderer->adapter, &caps) == WGPUStatus_Success &&
            caps.formatCount > 0 && caps.formats) {
            fmt = caps.formats[0];
        }
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        renderer->surface_format = fmt;
    }

    /* (Re)configure the surface when the framebuffer size changes. */
    if (!surface->configured || fb_w != surface->configured_w || fb_h != surface->configured_h) {
        WGPUSurfaceConfiguration config;
        memset(&config, 0, sizeof(config));
        config.device = renderer->device;
        config.format = renderer->surface_format;
        config.usage = WGPUTextureUsage_RenderAttachment;
        config.width = (uint32_t)fb_w;
        config.height = (uint32_t)fb_h;
        config.presentMode = WGPUPresentMode_Fifo;
        config.alphaMode = WGPUCompositeAlphaMode_Auto;
        wgpuSurfaceConfigure(surface->surface, &config);
        surface->configured = true;
        surface->configured_w = fb_w;
        surface->configured_h = fb_h;
    }

    /* Acquire the current swapchain texture. */
    WGPUSurfaceTexture surface_texture;
    memset(&surface_texture, 0, sizeof(surface_texture));
    wgpuSurfaceGetCurrentTexture(surface->surface, &surface_texture);
    if ((surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
         surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) ||
        !surface_texture.texture) {
        return NULL; /* not presentable this frame (resize/occlusion) — skip */
    }
    surface->texture = surface_texture.texture;
    surface->view = wgpuTextureCreateView(surface->texture, NULL);
    if (!surface->view) {
        wgpuTextureRelease(surface->texture);
        surface->texture = NULL;
        return NULL;
    }

    surface->encoder = wgpuDeviceCreateCommandEncoder(renderer->device, NULL);
    if (!surface->encoder) {
        wgpuTextureViewRelease(surface->view);
        surface->view = NULL;
        wgpuTextureRelease(surface->texture);
        surface->texture = NULL;
        return NULL;
    }

    WGPURenderPassColorAttachment color;
    memset(&color, 0, sizeof(color));
    color.view = surface->view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = (WGPUColor){surface->clear_color[0], surface->clear_color[1],
                                   surface->clear_color[2], surface->clear_color[3]};
    /*
     * API-version landmine: newer WGPURenderPassColorAttachment requires
     * .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED; older headers lack the field.
     */
#if defined(WGPU_DEPTH_SLICE_UNDEFINED)
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
#endif

    WGPURenderPassDescriptor rpd;
    memset(&rpd, 0, sizeof(rpd));
    rpd.colorAttachmentCount = 1;
    rpd.colorAttachments = &color;

    surface->pass_encoder = wgpuCommandEncoderBeginRenderPass(surface->encoder, &rpd);
    if (!surface->pass_encoder) {
        wgpuCommandEncoderRelease(surface->encoder);
        surface->encoder = NULL;
        wgpuTextureViewRelease(surface->view);
        surface->view = NULL;
        wgpuTextureRelease(surface->texture);
        surface->texture = NULL;
        return NULL;
    }

    /*
     * Advance this surface's frame ring index. Dynamic-buffer updates/binds this
     * frame target this index.
     */
    surface->frame_index = (surface->frame_index + 1u) % NE_WGPU_MAX_FRAMES_IN_FLIGHT;

    /* Reset per-pass draw state. */
    surface->current_topology = WGPUPrimitiveTopology_TriangleList;
    surface->current_index_buffer = NE_BUFFER_HANDLE_NULL;
    surface->current_index_format = WGPUIndexFormat_Uint16;

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

    if (surface->pass_encoder) {
        wgpuRenderPassEncoderEnd(surface->pass_encoder);
    }

    WGPUCommandBuffer cmd = NULL;
    if (surface->encoder) {
        cmd = wgpuCommandEncoderFinish(surface->encoder, NULL);
    }
    if (cmd) {
        wgpuQueueSubmit(renderer->queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
    }

    /*
     * No present call on the web — the browser presents when control returns to
     * it (after the rAF callback completes). A native Dawn build would call
     * wgpuSurfacePresent(surface->surface) here.
     */

    if (surface->pass_encoder) {
        wgpuRenderPassEncoderRelease(surface->pass_encoder);
        surface->pass_encoder = NULL;
    }
    if (surface->encoder) {
        wgpuCommandEncoderRelease(surface->encoder);
        surface->encoder = NULL;
    }
    if (surface->view) {
        wgpuTextureViewRelease(surface->view);
        surface->view = NULL;
    }
    if (surface->texture) {
        wgpuTextureRelease(surface->texture);
        surface->texture = NULL;
    }

    pass->surface = NULL;
}

/* ── Buffers ────────────────────────────────────────────────────────────── */

static WGPUBufferUsage ne_buffer_usage_to_wgpu(uint32_t usage) {
    /* All buffers can receive writes (wgpuQueueWriteBuffer / mappedAtCreation). */
    WGPUBufferUsage flags = WGPUBufferUsage_CopyDst;
    if (usage & NE_BUFFER_USAGE_VERTEX) {
        flags |= WGPUBufferUsage_Vertex;
    }
    if (usage & NE_BUFFER_USAGE_INDEX) {
        flags |= WGPUBufferUsage_Index;
    }
    if (usage & NE_BUFFER_USAGE_UNIFORM) {
        flags |= WGPUBufferUsage_Uniform;
    }
    if (usage & NE_BUFFER_USAGE_STORAGE) {
        flags |= WGPUBufferUsage_Storage;
    }
    return flags;
}

NEBufferHandle ne_buffer_create(NERenderer *renderer, const NEBufferDesc *desc) {
    if (!renderer || !desc || desc->size == 0) {
        return NE_BUFFER_HANDLE_NULL;
    }
    if (!desc->usage) {
        NE_LOG_ERROR("buffer creation requires at least one usage flag");
        return NE_BUFFER_HANDLE_NULL;
    }
    if (!renderer->device_ready) {
        NE_LOG_WARN("ne_buffer_create called before WebGPU device ready");
        return NE_BUFFER_HANDLE_NULL;
    }

    uint32_t index = ne_pool_alloc((void **)&renderer->buffers, &renderer->buffer_count,
                                   &renderer->buffer_cap, sizeof(NEBufferSlot));
    if (index == UINT32_MAX) {
        NE_LOG_ERROR("buffer pool allocation failed");
        return NE_BUFFER_HANDLE_NULL;
    }

    NEBufferSlot *slot = &renderer->buffers[index];
    slot->occupied = true;
    slot->dynamic = desc->dynamic;
    slot->usage = desc->usage;
    slot->size = desc->size;

    const WGPUBufferUsage wgpu_usage = ne_buffer_usage_to_wgpu(desc->usage);
    const uint32_t copy_count = ne_buffer_slot_copy_count(slot);

    for (uint32_t i = 0; i < copy_count; i++) {
        WGPUBufferDescriptor bd;
        memset(&bd, 0, sizeof(bd));
        bd.usage = wgpu_usage;
        bd.size = desc->size;
        bd.mappedAtCreation = (desc->initial_data != NULL);

        WGPUBuffer buffer = wgpuDeviceCreateBuffer(renderer->device, &bd);
        if (!buffer) {
            NE_LOG_ERROR("failed to create WebGPU buffer (%u bytes)", desc->size);
            ne_buffer_slot_release(slot); /* releases copies created so far */
            return NE_BUFFER_HANDLE_NULL;
        }

        if (desc->initial_data) {
            void *mapped = wgpuBufferGetMappedRange(buffer, 0, desc->size);
            if (mapped) {
                memcpy(mapped, desc->initial_data, desc->size);
            }
            wgpuBufferUnmap(buffer);
        }

        slot->copies[i] = buffer;
    }

    return (NEBufferHandle){.id = index + 1};
}

static WGPUBuffer ne_buffer_get_for_frame(const NERenderer *renderer, NEBufferHandle handle,
                                          uint32_t frame_index) {
    if (!ne_buffer_handle_valid(handle)) {
        return NULL;
    }
    uint32_t index = handle.id - 1;
    if (index >= renderer->buffer_cap || !renderer->buffers[index].occupied) {
        return NULL;
    }
    const NEBufferSlot *slot = &renderer->buffers[index];
    uint32_t copy = frame_index % ne_buffer_slot_copy_count(slot);
    return slot->copies[copy];
}

/* Validate a buffer slot for an update and bounds-check the region. */
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

void ne_buffer_update(NERenderer *renderer, NEBufferHandle handle,
                      const void *data, uint32_t size, uint32_t offset) {
    NEBufferSlot *slot = ne_buffer_update_validate(renderer, handle, data, size, offset);
    if (!slot) {
        return;
    }

    /*
     * Renderer-scoped update: intended for static/one-time setup (no frame
     * context). For a dynamic buffer we have no surface frame_index here, so we
     * write ALL copies to keep them consistent. Per-frame dynamic updates must
     * use ne_render_pass_update_buffer instead.
     */
    if (slot->dynamic) {
        NE_LOG_WARN("ne_buffer_update on a dynamic buffer writes all copies; "
                    "use ne_render_pass_update_buffer for per-frame updates");
    }
    const uint32_t copy_count = ne_buffer_slot_copy_count(slot);
    for (uint32_t i = 0; i < copy_count; i++) {
        wgpuQueueWriteBuffer(renderer->queue, slot->copies[i], offset, data, size);
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
    if (renderer->buffer_count > 0) {
        renderer->buffer_count--;
    }
}

/* ── Shaders ────────────────────────────────────────────────────────────── */

void ne_renderer_set_shader_optimization(NERenderer *renderer, NEShaderOptimization level) {
    if (!renderer) {
        return;
    }
    /* WGSL compilation exposes no app-facing optimisation knob in the browser;
     * stored for parity with other backends but otherwise unused. */
    renderer->shader_optimization = level;
}

NEShaderHandle ne_shader_create(NERenderer *renderer, const NEShaderDesc *desc) {
    (void)desc;
    if (!renderer) {
        return NE_SHADER_HANDLE_NULL;
    }
    /* The browser has no portable shader-bytecode path (SPIR-V passthrough is
     * not available in WebGPU); use ne_shader_create_from_source with WGSL. */
    NE_LOG_WARN("bytecode shaders unsupported on WebGPU; use ne_shader_create_from_source with WGSL");
    return NE_SHADER_HANDLE_NULL;
}

NEShaderHandle ne_shader_create_from_source(NERenderer *renderer, const NEShaderSourceDesc *desc) {
    if (!renderer || !desc || !desc->source || !desc->entry_point) {
        return NE_SHADER_HANDLE_NULL;
    }
    if (!renderer->device_ready) {
        NE_LOG_WARN("ne_shader_create_from_source called before WebGPU device ready");
        return NE_SHADER_HANDLE_NULL;
    }

    WGPUShaderSourceWGSL wgsl_desc;
    memset(&wgsl_desc, 0, sizeof(wgsl_desc));
    wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_desc.code = ne_sv(desc->source);

    WGPUShaderModuleDescriptor module_desc;
    memset(&module_desc, 0, sizeof(module_desc));
    module_desc.nextInChain = (WGPUChainedStruct *)&wgsl_desc;

    WGPUShaderModule module = wgpuDeviceCreateShaderModule(renderer->device, &module_desc);
    if (!module) {
        NE_LOG_ERROR("failed to create WGSL shader module%s%s",
                     desc->filename ? " " : "", desc->filename ? desc->filename : "");
        return NE_SHADER_HANDLE_NULL;
    }

    uint32_t index = ne_pool_alloc((void **)&renderer->shaders, &renderer->shader_count,
                                   &renderer->shader_cap, sizeof(NEShaderSlot));
    if (index == UINT32_MAX) {
        NE_LOG_ERROR("shader pool allocation failed");
        wgpuShaderModuleRelease(module);
        return NE_SHADER_HANDLE_NULL;
    }

    NEShaderSlot *slot = &renderer->shaders[index];
    slot->occupied = true;
    slot->stage = desc->stage;
    slot->module = module;
    slot->entry_point = ne_strdup(desc->entry_point);

    return (NEShaderHandle){.id = index + 1};
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
    if (renderer->shader_count > 0) {
        renderer->shader_count--;
    }
}

static const NEShaderSlot *ne_shader_get_slot(const NERenderer *renderer, NEShaderHandle handle) {
    if (!ne_shader_handle_valid(handle)) {
        return NULL;
    }
    uint32_t index = handle.id - 1;
    if (index >= renderer->shader_cap || !renderer->shaders[index].occupied) {
        return NULL;
    }
    return &renderer->shaders[index];
}

/* ── Pipelines ──────────────────────────────────────────────────────────── */

static WGPUVertexFormat ne_vertex_format_to_wgpu(NEVertexFormat fmt) {
    switch (fmt) {
    case NE_VERTEX_FORMAT_FLOAT:    return WGPUVertexFormat_Float32;
    case NE_VERTEX_FORMAT_FLOAT2:   return WGPUVertexFormat_Float32x2;
    case NE_VERTEX_FORMAT_FLOAT3:   return WGPUVertexFormat_Float32x3;
    case NE_VERTEX_FORMAT_FLOAT4:   return WGPUVertexFormat_Float32x4;
    case NE_VERTEX_FORMAT_UNORM8X4: return WGPUVertexFormat_Unorm8x4;
    default:                        return WGPUVertexFormat_Float32x4;
    }
}

static WGPUPrimitiveTopology ne_topology_to_wgpu(NEPrimitiveTopology t) {
    switch (t) {
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return WGPUPrimitiveTopology_TriangleList;
    case NE_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return WGPUPrimitiveTopology_TriangleStrip;
    case NE_PRIMITIVE_TOPOLOGY_LINE_LIST:      return WGPUPrimitiveTopology_LineList;
    case NE_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return WGPUPrimitiveTopology_LineStrip;
    case NE_PRIMITIVE_TOPOLOGY_POINT_LIST:     return WGPUPrimitiveTopology_PointList;
    default:                                   return WGPUPrimitiveTopology_TriangleList;
    }
}

static WGPUBlendFactor ne_blend_factor_to_wgpu(NEBlendFactor f) {
    switch (f) {
    case NE_BLEND_FACTOR_ZERO:                return WGPUBlendFactor_Zero;
    case NE_BLEND_FACTOR_ONE:                 return WGPUBlendFactor_One;
    case NE_BLEND_FACTOR_SRC_ALPHA:           return WGPUBlendFactor_SrcAlpha;
    case NE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return WGPUBlendFactor_OneMinusSrcAlpha;
    case NE_BLEND_FACTOR_DST_ALPHA:           return WGPUBlendFactor_DstAlpha;
    case NE_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return WGPUBlendFactor_OneMinusDstAlpha;
    default:                                  return WGPUBlendFactor_One;
    }
}

static WGPUBlendOperation ne_blend_op_to_wgpu(NEBlendOp op) {
    switch (op) {
    case NE_BLEND_OP_ADD:              return WGPUBlendOperation_Add;
    case NE_BLEND_OP_SUBTRACT:         return WGPUBlendOperation_Subtract;
    case NE_BLEND_OP_REVERSE_SUBTRACT: return WGPUBlendOperation_ReverseSubtract;
    case NE_BLEND_OP_MIN:              return WGPUBlendOperation_Min;
    case NE_BLEND_OP_MAX:              return WGPUBlendOperation_Max;
    default:                           return WGPUBlendOperation_Add;
    }
}

/* WebGPU caps vertex attributes per buffer; this bound keeps the stack arrays
 * fixed-size. Raise if a pipeline ever needs more. */
enum { NE_WGPU_MAX_VERTEX_ATTRIBUTES = 16 };

NEPipelineHandle ne_pipeline_create(NERenderer *renderer, const NEPipelineDesc *desc) {
    if (!renderer || !desc) {
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (!renderer->device_ready) {
        NE_LOG_WARN("ne_pipeline_create called before WebGPU device ready");
        return NE_PIPELINE_HANDLE_NULL;
    }
    if (renderer->surface_format == WGPUTextureFormat_Undefined) {
        NE_LOG_ERROR("ne_pipeline_create: surface format not yet known (create the pipeline after the first frame begins)");
        return NE_PIPELINE_HANDLE_NULL;
    }

    const NEShaderSlot *vs = ne_shader_get_slot(renderer, desc->vertex_shader);
    const NEShaderSlot *fs = ne_shader_get_slot(renderer, desc->fragment_shader);
    if (!vs || !fs) {
        NE_LOG_ERROR("pipeline creation failed: invalid vertex or fragment shader");
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* Build vertex buffer layouts + attributes (fixed-size stack storage). */
    WGPUVertexBufferLayout vb_layouts[NE_WGPU_MAX_VERTEX_ATTRIBUTES];
    WGPUVertexAttribute attrs[NE_WGPU_MAX_VERTEX_ATTRIBUTES][NE_WGPU_MAX_VERTEX_ATTRIBUTES];
    memset(vb_layouts, 0, sizeof(vb_layouts));
    memset(attrs, 0, sizeof(attrs));

    uint32_t layout_count = 0;
    if (desc->vertex_layouts && desc->vertex_layout_count > 0) {
        layout_count = desc->vertex_layout_count;
        if (layout_count > NE_WGPU_MAX_VERTEX_ATTRIBUTES) {
            NE_LOG_WARN("pipeline has %u vertex layouts; clamping to %d",
                        layout_count, NE_WGPU_MAX_VERTEX_ATTRIBUTES);
            layout_count = NE_WGPU_MAX_VERTEX_ATTRIBUTES;
        }
        for (uint32_t b = 0; b < layout_count; b++) {
            const NEVertexBufferLayout *layout = &desc->vertex_layouts[b];
            uint32_t attr_count = layout->attribute_count;
            if (attr_count > NE_WGPU_MAX_VERTEX_ATTRIBUTES) {
                attr_count = NE_WGPU_MAX_VERTEX_ATTRIBUTES;
            }
            for (uint32_t a = 0; a < attr_count; a++) {
                const NEVertexAttribute *src = &layout->attributes[a];
                attrs[b][a].format = ne_vertex_format_to_wgpu(src->format);
                attrs[b][a].offset = src->offset;
                attrs[b][a].shaderLocation = src->location;
            }
            vb_layouts[b].arrayStride = layout->stride;
            vb_layouts[b].stepMode = WGPUVertexStepMode_Vertex;
            vb_layouts[b].attributeCount = attr_count;
            vb_layouts[b].attributes = attrs[b];
        }
    }

    /* Blend state (optional). */
    WGPUBlendState blend;
    memset(&blend, 0, sizeof(blend));
    if (desc->blend.enabled) {
        blend.color.srcFactor = ne_blend_factor_to_wgpu(desc->blend.src_color);
        blend.color.dstFactor = ne_blend_factor_to_wgpu(desc->blend.dst_color);
        blend.color.operation = ne_blend_op_to_wgpu(desc->blend.color_op);
        blend.alpha.srcFactor = ne_blend_factor_to_wgpu(desc->blend.src_alpha);
        blend.alpha.dstFactor = ne_blend_factor_to_wgpu(desc->blend.dst_alpha);
        blend.alpha.operation = ne_blend_op_to_wgpu(desc->blend.alpha_op);
    }

    WGPUColorTargetState color_target;
    memset(&color_target, 0, sizeof(color_target));
    color_target.format = renderer->surface_format;
    color_target.blend = desc->blend.enabled ? &blend : NULL;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment_state;
    memset(&fragment_state, 0, sizeof(fragment_state));
    fragment_state.module = fs->module;
    fragment_state.entryPoint = ne_sv(fs->entry_point);
    fragment_state.targetCount = 1;
    fragment_state.targets = &color_target;

    WGPURenderPipelineDescriptor pd;
    memset(&pd, 0, sizeof(pd));
    pd.vertex.module = vs->module;
    pd.vertex.entryPoint = ne_sv(vs->entry_point);
    pd.vertex.bufferCount = layout_count;
    pd.vertex.buffers = layout_count > 0 ? vb_layouts : NULL;
    pd.primitive.topology = ne_topology_to_wgpu(desc->topology);
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.primitive.cullMode = WGPUCullMode_None;
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fragment_state;

    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(renderer->device, &pd);
    if (!pipeline) {
        NE_LOG_ERROR("failed to create WebGPU render pipeline");
        return NE_PIPELINE_HANDLE_NULL;
    }

    uint32_t index = ne_pool_alloc((void **)&renderer->pipelines, &renderer->pipeline_count,
                                   &renderer->pipeline_cap, sizeof(NEPipelineSlot));
    if (index == UINT32_MAX) {
        NE_LOG_ERROR("pipeline pool allocation failed");
        wgpuRenderPipelineRelease(pipeline);
        return NE_PIPELINE_HANDLE_NULL;
    }

    NEPipelineSlot *slot = &renderer->pipelines[index];
    slot->occupied = true;
    slot->pipeline = pipeline;
    slot->topology = ne_topology_to_wgpu(desc->topology);

    return (NEPipelineHandle){.id = index + 1};
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
    ne_pipeline_slot_release(&renderer->pipelines[index]);
    if (renderer->pipeline_count > 0) {
        renderer->pipeline_count--;
    }
}

NEComputePipelineHandle ne_compute_pipeline_create(NERenderer *renderer, const NEComputePipelineDesc *desc) {
    (void)renderer;
    (void)desc;
    NE_LOG_WARN("compute pipelines not yet implemented on WebGPU");
    return NE_COMPUTE_PIPELINE_HANDLE_NULL;
}

void ne_compute_pipeline_destroy(NERenderer *renderer, NEComputePipelineHandle handle) {
    (void)renderer;
    (void)handle;
}

/* ── Render pass commands ───────────────────────────────────────────────── */

static WGPURenderPassEncoder ne_pass_get_encoder(NERenderPass *pass) {
    if (!pass || !pass->surface || !pass->surface->pass_encoder) {
        return NULL;
    }
    return pass->surface->pass_encoder;
}

void ne_render_pass_set_pipeline(NERenderPass *pass, NEPipelineHandle pipeline) {
    WGPURenderPassEncoder enc = ne_pass_get_encoder(pass);
    if (!enc) {
        return;
    }
    NERenderer *renderer = pass->surface->renderer;
    uint32_t index = pipeline.id - 1;
    if (!ne_pipeline_handle_valid(pipeline) || index >= renderer->pipeline_cap ||
        !renderer->pipelines[index].occupied) {
        NE_LOG_WARN("set_pipeline: invalid pipeline handle (id=%u)", pipeline.id);
        return;
    }
    NEPipelineSlot *slot = &renderer->pipelines[index];
    wgpuRenderPassEncoderSetPipeline(enc, slot->pipeline);
    pass->surface->current_topology = slot->topology;
}

void ne_render_pass_set_vertex_buffer(NERenderPass *pass, uint64_t slot, NEBufferHandle buffer) {
    WGPURenderPassEncoder enc = ne_pass_get_encoder(pass);
    if (!enc) {
        return;
    }
    WGPUBuffer buf = ne_buffer_get_for_frame(pass->surface->renderer, buffer, pass->surface->frame_index);
    if (!buf) {
        NE_LOG_WARN("set_vertex_buffer: invalid buffer handle (id=%u)", buffer.id);
        return;
    }
    wgpuRenderPassEncoderSetVertexBuffer(enc, (uint32_t)slot, buf, 0, WGPU_WHOLE_SIZE);
}

void ne_render_pass_set_index_buffer(NERenderPass *pass, NEBufferHandle buffer, NEIndexType type) {
    WGPURenderPassEncoder enc = ne_pass_get_encoder(pass);
    if (!enc) {
        return;
    }
    WGPUBuffer buf = ne_buffer_get_for_frame(pass->surface->renderer, buffer, pass->surface->frame_index);
    if (!buf) {
        NE_LOG_WARN("set_index_buffer: invalid buffer handle (id=%u)", buffer.id);
        return;
    }
    WGPUIndexFormat fmt = (type == NE_INDEX_TYPE_UINT32) ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
    pass->surface->current_index_buffer = buffer;
    pass->surface->current_index_format = fmt;
    wgpuRenderPassEncoderSetIndexBuffer(enc, buf, fmt, 0, WGPU_WHOLE_SIZE);
}

void ne_render_pass_set_uniform_data(NERenderPass *pass, NEShaderStage stage,
                                     uint64_t slot, const void *data, size_t size) {
    (void)pass;
    (void)stage;
    (void)slot;
    (void)data;
    (void)size;
    /*
     * WebGPU has no push-constants / setBytes equivalent. Inline uniforms must
     * go through a uniform buffer + bind group, which this backend does not yet
     * implement. Documented gap; the triangle demo does not use this path.
     */
    NE_LOG_WARN("set_uniform_data not supported on WebGPU; use a uniform buffer + bind group");
}

void ne_render_pass_update_buffer(NERenderPass *pass, NEBufferHandle handle,
                                  const void *data, uint32_t size, uint32_t offset) {
    if (!pass || !pass->surface) {
        return;
    }
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
     * Write only the copy bound for this frame. The browser does not run more
     * than a frame ahead via rAF, so the GPU is not reading this copy; the write
     * neither stalls nor races. A dynamic buffer must be updated every frame
     * before it is drawn — the other copies hold prior frames' data.
     */
    uint32_t copy = pass->surface->frame_index % ne_buffer_slot_copy_count(slot);
    wgpuQueueWriteBuffer(renderer->queue, slot->copies[copy], offset, data, size);
}

void ne_render_pass_draw(NERenderPass *pass, uint64_t first_vertex, uint64_t vertex_count) {
    WGPURenderPassEncoder enc = ne_pass_get_encoder(pass);
    if (!enc) {
        return;
    }
    wgpuRenderPassEncoderDraw(enc, (uint32_t)vertex_count, 1, (uint32_t)first_vertex, 0);
}

void ne_render_pass_draw_indexed(NERenderPass *pass, uint64_t index_count,
                                 uint64_t first_index, int64_t vertex_offset) {
    WGPURenderPassEncoder enc = ne_pass_get_encoder(pass);
    if (!enc) {
        return;
    }
    if (!pass->surface->current_index_buffer.id) {
        NE_LOG_WARN("draw_indexed called without a bound index buffer");
        return;
    }
    wgpuRenderPassEncoderDrawIndexed(enc, (uint32_t)index_count, 1, (uint32_t)first_index,
                                     (int32_t)vertex_offset, 0);
}

/* ── Compute pass stubs (match Metal backend) ───────────────────────────── */

NEComputePass *ne_render_pass_begin_compute(NERenderPass *pass) {
    (void)pass;
    NE_LOG_WARN("compute passes not yet implemented on WebGPU");
    return NULL;
}

void ne_render_pass_end_compute(NERenderPass *pass, NEComputePass *compute) {
    (void)pass;
    (void)compute;
}

void ne_compute_pass_set_pipeline(NEComputePass *pass, NEComputePipelineHandle pipeline) {
    (void)pass;
    (void)pipeline;
}

void ne_compute_pass_set_storage_buffer(NEComputePass *pass, uint64_t slot, NEBufferHandle buffer) {
    (void)pass;
    (void)slot;
    (void)buffer;
}

void ne_compute_pass_set_uniform_data(NEComputePass *pass, uint64_t slot,
                                      const void *data, uint32_t size) {
    (void)pass;
    (void)slot;
    (void)data;
    (void)size;
}

void ne_compute_pass_dispatch(NEComputePass *pass, uint64_t group_count_x,
                              uint64_t group_count_y, uint64_t group_count_z) {
    (void)pass;
    (void)group_count_x;
    (void)group_count_y;
    (void)group_count_z;
}

/* ── Image stubs (match Metal backend) ──────────────────────────────────── */

NEImageHandle ne_image_create(NERenderer *renderer, const NEImageDesc *desc) {
    (void)renderer;
    (void)desc;
    NE_LOG_WARN("images not yet implemented on WebGPU");
    return NE_IMAGE_HANDLE_NULL;
}

void ne_image_update(NERenderer *renderer, NEImageHandle handle, const void *data, uint32_t size) {
    (void)renderer;
    (void)handle;
    (void)data;
    (void)size;
}

void ne_image_destroy(NERenderer *renderer, NEImageHandle handle) {
    (void)renderer;
    (void)handle;
}
