#include "ne_frame.h"

#include "ne_renderer_pass.h"

void ne_render_frame(void *user) {
    NEFrameContext *ctx = (NEFrameContext *)user;
    if (!ctx) {
        return;
    }

    NERenderPass *pass = ne_renderer_begin_frame(ctx->renderer, ctx->surface);
    if (!pass) {
        return;
    }

    if (ctx->on_update) {
        ctx->on_update(ctx, pass);
    }

    ne_render_pass_set_pipeline(pass, ctx->pipeline);
    ne_render_pass_set_vertex_buffer(pass, 0, ctx->vertex_buffer);
    ne_render_pass_draw(pass, 0, ctx->vertex_count);

    ne_renderer_end_frame(ctx->renderer, pass);
}
