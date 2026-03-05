#ifndef NE_RENDERER_H
#define NE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to the application instance. */
typedef struct NEApp NEApp;

/** Opaque handle to a platform window. */
typedef struct NEWindow NEWindow;

/** Opaque handle to a renderer instance. */
typedef struct NERenderer NERenderer;

/** Opaque handle to a window presentation surface. */
typedef struct NERenderSurface NERenderSurface;

/**
 * Rendering backend.
 *
 * The public API is designed to be extensible to additional backends.
 */
typedef enum NERenderBackend {
    /** Apple Metal backend (macOS). */
    NE_RENDER_BACKEND_METAL = 0,

    /** Vulkan backend (Windows). */
    NE_RENDER_BACKEND_VULKAN = 1,
} NERenderBackend;

/**
 * Renderer creation parameters.
 */
typedef struct NERendererDesc {
    /** Backend to use. Backend support is platform-dependent. */
    NERenderBackend backend;

    /** Enable debug/validation features when available (best-effort). */
    bool enable_validation;
} NERendererDesc;

/**
 * Presentation surface creation parameters.
 */
typedef struct NERenderSurfaceDesc {
    /** Enable vertical sync (best-effort; backend-dependent). */
    bool vsync;

    /** Default clear color used by the simple demo rendering path. */
    float clear_color_rgba[4];
} NERenderSurfaceDesc;

/**
 * Create the renderer.
 *
 * Notes:
 * - The engine currently supports only a single renderer instance for the
 *   lifetime of the application.
 * - Attempting to create more than one renderer will fail.
 *
 * Parameters:
 * - `app`: Application instance returned by `ne_app_create`.
 * - `desc`: Renderer description (may be NULL for defaults).
 *
 * Returns:
 * - A valid `NERenderer*` on success.
 * - NULL on failure.
 */
NERenderer *ne_renderer_create(NEApp *app, const NERendererDesc *desc);

/**
 * Destroy the renderer.
 *
 * Parameters:
 * - `renderer`: Renderer instance (may be NULL).
 */
void ne_renderer_destroy(NERenderer *renderer);

/**
 * Create a presentation surface for a window.
 *
 * A surface represents the renderer's ability to present to a given window.
 * Exactly one surface may exist per window.
 *
 * Parameters:
 * - `renderer`: Renderer instance.
 * - `window`: Window to render/present to.
 * - `desc`: Surface configuration (may be NULL for defaults).
 *
 * Returns:
 * - A valid `NERenderSurface*` on success.
 * - NULL on failure (including if the window already has a surface).
 */
NERenderSurface *ne_renderer_create_surface(NERenderer *renderer, NEWindow *window, const NERenderSurfaceDesc *desc);

/**
 * Destroy a presentation surface.
 *
 * Parameters:
 * - `renderer`: Renderer instance.
 * - `surface`: Surface instance (may be NULL).
 */
void ne_renderer_destroy_surface(NERenderer *renderer, NERenderSurface *surface);

/**
 * Set the clear color for a surface.
 *
 * Parameters:
 * - `surface`: Surface instance.
 * - `r`, `g`, `b`, `a`: RGBA values in [0..1].
 */
void ne_renderer_surface_set_clear_color(NERenderSurface *surface, float r, float g, float b, float a);

/**
 * Begin rendering a frame for a given surface.
 *
 * The backend uses pull-based resizing: it queries the window framebuffer size
 * during `ne_renderer_begin_frame` and updates presentation resources as
 * needed.
 *
 * Parameters:
 * - `renderer`: Renderer instance.
 * - `surface`: Surface instance.
 *
 * Returns:
 * - `true` if a drawable/backbuffer was acquired and rendering can proceed.
 * - `false` if the surface/window is not ready (e.g., minimized, closed).
 */
bool ne_renderer_begin_frame(NERenderer *renderer, NERenderSurface *surface);

/**
 * End rendering a frame for a given surface.
 *
 * This typically presents the rendered image.
 *
 * Parameters:
 * - `renderer`: Renderer instance.
 * - `surface`: Surface instance.
 */
void ne_renderer_end_frame(NERenderer *renderer, NERenderSurface *surface);

#ifdef __cplusplus
}
#endif

#endif
