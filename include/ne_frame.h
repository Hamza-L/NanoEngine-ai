#ifndef NE_FRAME_H
#define NE_FRAME_H

#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Everything needed to draw and present one frame.
 *
 * This is the single source of truth for "what a frame draws" so the same
 * sequence runs from the main loop and from a resize/paint callback without
 * duplicating renderer calls.
 */
typedef struct NEFrameContext {
    NERenderer *renderer;
    NERenderSurface *surface;
    NEPipelineHandle pipeline;
    NEBufferHandle vertex_buffer;
    uint32_t vertex_count;
} NEFrameContext;

/**
 * Draw and present exactly one frame.
 *
 * `user` must point to a valid `NEFrameContext`.  The `void *` signature lets
 * this be registered directly as a window render dispatch
 * (see `ne_set_window_render_dispatch`) so the platform layer can trigger a
 * redraw during a live resize while depending only on this slim header — never
 * on the renderer backend.
 *
 * Safe to call when no drawable is available (e.g. minimized): the underlying
 * `ne_renderer_begin_frame` returns NULL and the call becomes a no-op.
 */
void ne_render_frame(void *user);

#ifdef __cplusplus
}
#endif

#endif /* NE_FRAME_H */
