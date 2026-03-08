#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_window.h"

#include <stdlib.h>
#include <string.h>

struct NERenderer {
    void *device;
    void *queue;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;

    void *layer; /* CAMetalLayer* */
    void *drawable; /* id<CAMetalDrawable> */
    void *command_buffer; /* id<MTLCommandBuffer> */

    float clear_color[4];
};

static NERenderer *g_renderer_singleton = NULL;

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

static id<MTLDevice> ne_renderer_get_device(const NERenderer *renderer) {
    return renderer && renderer->device ? (__bridge id<MTLDevice>)renderer->device : nil;
}

static id<MTLCommandQueue> ne_renderer_get_queue(const NERenderer *renderer) {
    return renderer && renderer->queue ? (__bridge id<MTLCommandQueue>)renderer->queue : nil;
}

NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc) {
    (void)app;

    if (g_renderer_singleton) {
        NE_LOG_ERROR("renderer already created (only one renderer is supported)");
        return NULL;
    }

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

    g_renderer_singleton = renderer;
    return renderer;
}

void ne_renderer_destroy(NERenderer *renderer) {
    if (!renderer) {
        return;
    }

    if (renderer != g_renderer_singleton) {
        NE_LOG_WARN("attempted to destroy non-singleton renderer");
    }

    if (renderer->queue) {
        (void)CFBridgingRelease(renderer->queue);
        renderer->queue = NULL;
    }

    if (renderer->device) {
        (void)CFBridgingRelease(renderer->device);
        renderer->device = NULL;
    }

    if (renderer == g_renderer_singleton) {
        g_renderer_singleton = NULL;
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

    /* Enforce one surface per window by associating a marker with the view. */
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

bool ne_renderer_begin_frame(NERenderer *renderer, NERenderSurface *surface) {
    if (!renderer || !surface || surface->renderer != renderer) {
        return false;
    }

    if (!surface->window || !ne_window_is_open(surface->window)) {
        return false;
    }

    if (surface->drawable || surface->command_buffer) {
        NE_LOG_WARN("begin_frame called while a frame is already in progress");
        return false;
    }

    CAMetalLayer *layer = ne_surface_get_layer(surface);
    if (!layer) {
        return false;
    }

    int32_t fb_w = 0;
    int32_t fb_h = 0;
    if (!ne_window_get_framebuffer_size(surface->window, &fb_w, &fb_h)) {
        return false;
    }
    if (fb_w <= 0 || fb_h <= 0) {
        return false;
    }

    layer.drawableSize = CGSizeMake((CGFloat)fb_w, (CGFloat)fb_h);

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) {
        return false;
    }

    id<MTLCommandQueue> queue = ne_renderer_get_queue(renderer);
    if (!queue) {
        return false;
    }

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    if (!command_buffer) {
        return false;
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

    return true;
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

    [command_buffer presentDrawable:drawable];
    [command_buffer commit];

    (void)CFBridgingRelease(surface->command_buffer);
    surface->command_buffer = NULL;

    (void)CFBridgingRelease(surface->drawable);
    surface->drawable = NULL;
}
