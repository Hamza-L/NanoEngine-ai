#ifndef NE_FRAME_H
#define NE_FRAME_H

#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_pass.h"
#include "ne_renderer_pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NEFrameContext;

/**
 * Optional per-frame update hook, invoked after the frame begins but before the
 * draw, with the active render pass. Use it to update dynamic buffers for this
 * frame (e.g. via `ne_render_pass_update_buffer`).
 */
typedef void (*NEFrameUpdateFn)(struct NEFrameContext *ctx, NERenderPass *pass);

/**
 * Everything needed to draw and present one frame.
 *
 * This is the single source of truth for "what a frame draws", invoked by
 * whatever drives frames on the platform — a display link on macOS (push), the
 * main loop on Win32 (pull) — without duplicating renderer calls.
 */
typedef struct NEFrameContext {
    NERenderer *renderer;
    NERenderSurface *surface;
    NEPipelineHandle pipeline;
    NEBufferHandle vertex_buffer;
    uint32_t vertex_count;

    /** Optional per-frame update callback (may be NULL). */
    NEFrameUpdateFn on_update;
    /** Opaque user pointer passed through to `on_update` via the context. */
    void *user;
} NEFrameContext;

/**
 * Draw and present exactly one frame.
 *
 * `user` must point to a valid `NEFrameContext`.  The `void *` signature lets
 * this be registered directly as a window render dispatch
 * (see `ne_set_window_render_dispatch`) so the platform's frame driver can
 * invoke it while depending only on this slim header — never on the renderer
 * backend.
 *
 * Safe to call when no drawable is available (e.g. minimized): the underlying
 * `ne_renderer_begin_frame` returns NULL and the call becomes a no-op.
 */
void ne_render_frame(void *user);

#ifdef __cplusplus
}
#endif

#endif /* NE_FRAME_H */
