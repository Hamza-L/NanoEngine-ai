#include "ne_renderer.h"

#include "ne_log.h"

#include <stdlib.h>

struct NERenderer {
    NERenderBackend backend;
};

struct NERenderSurface {
    NERenderer *renderer;
    NEWindow *window;
    float clear_color[4];
};

NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc) {
    (void)app;

    const NERenderBackend backend = desc ? desc->backend : NE_RENDER_BACKEND_METAL;
    (void)backend;

    /*
     * Stub implementation for platforms where no renderer backend is compiled.
     * This keeps the engine linkable while we bring up platform/windowing.
     */
    NE_LOG_ERROR("no renderer backend compiled for this platform (requested backend=%d)", (int)backend);
    return NULL;
}

void ne_renderer_destroy(NERenderer *renderer) {
    free(renderer);
}

NERenderSurface *ne_renderer_create_surface(NERenderer *renderer, NEWindow *window, const NERenderSurfaceDesc *desc) {
    (void)renderer;
    (void)window;
    (void)desc;
    return NULL;
}

void ne_renderer_destroy_surface(NERenderer *renderer, NERenderSurface *surface) {
    (void)renderer;
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
    (void)renderer;
    (void)surface;
    return false;
}

void ne_renderer_end_frame(NERenderer *renderer, NERenderSurface *surface) {
    (void)renderer;
    (void)surface;
}
