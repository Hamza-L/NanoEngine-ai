#include "ne_app.h"
#include "ne_file.h"
#include "ne_frame.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_renderer_pass.h"
#include "ne_renderer_pipeline.h"
#include "ne_renderer_shader.h"
#include "ne_window.h"

#include "test/ne_test.h"

#include <math.h>

/* ── Callbacks ──────────────────────────────────────────────────────────── */

static void on_key_down(NEWindow *window, NEKeyEvent event, void *user_data) {
    (void)user_data;
    if (event.key == NE_KEY_ESCAPE) {
        ne_window_request_close(window);
    }
}

/* ── Triangle data ──────────────────────────────────────────────────────── */

typedef struct Vertex {
    float position[2];
    float color[4];
} Vertex;

static const Vertex k_triangle_vertices[] = {
    {{ 0.0f,  0.5f}, {1.0f, 0.0f, 0.0f, 1.0f}},  /* top    — red   */
    {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f, 1.0f}},  /* left   — green */
    {{ 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f, 1.0f}},  /* right  — blue  */
};

/* ── Dynamic-buffer demo state ──────────────────────────────────────────── */
/*
 * Proves per-frame dynamic buffering: every frame we rotate the base triangle
 * and write the result into the dynamic vertex buffer via
 * ne_render_pass_update_buffer. The triangle spins smoothly with no flicker or
 * tearing — which only holds if each frame writes its own buffer copy while the
 * GPU still reads the previous frame's copy.
 */

typedef struct DemoState {
    float angle; /* radians, advanced each frame */
} DemoState;

static NEPipelineHandle create_basic_pipeline(NERenderer *renderer);

static void on_frame_update(NEFrameContext *ctx, NERenderPass *pass) {
    /*
     * On the web the GPU device is acquired asynchronously, so resources cannot
     * be created in main() before the loop (the device isn't ready yet). Create
     * them lazily on the first frame that runs (which only happens once the
     * device is ready, and after begin_frame has chosen the surface format that
     * pipeline creation needs). On native the resources were created eagerly, so
     * the handles are already valid and this block is a no-op.
     */

    if (!ne_pipeline_handle_valid(ctx->pipeline)) {
        ctx->pipeline = create_basic_pipeline(ctx->renderer);
        ctx->vertex_buffer = ne_buffer_create(ctx->renderer, &(NEBufferDesc){
            .size = sizeof(k_triangle_vertices),
            .usage = NE_BUFFER_USAGE_VERTEX,
            .initial_data = k_triangle_vertices,
            .dynamic = true,
        });
        ctx->vertex_count = 3;
    }
    if (!ne_pipeline_handle_valid(ctx->pipeline) || !ne_buffer_handle_valid(ctx->vertex_buffer)) {
        return; /* resources not ready / failed — skip this frame's update */
    }

    DemoState *state = (DemoState *)ctx->user;
    state->angle += 0.02f;
    const float c = cosf(state->angle);
    const float s = sinf(state->angle);

    Vertex rotated[3];
    for (int i = 0; i < 3; i++) {
        const float x = k_triangle_vertices[i].position[0];
        const float y = k_triangle_vertices[i].position[1];
        rotated[i].position[0] = x * c - y * s;
        rotated[i].position[1] = x * s + y * c;
        for (int k = 0; k < 4; k++) {
            rotated[i].color[k] = k_triangle_vertices[i].color[k];
        }
    }

    ne_render_pass_update_buffer(pass, ctx->vertex_buffer, rotated, sizeof(rotated), 0);
}


static NEPipelineHandle create_basic_pipeline(NERenderer *renderer) {
    NEShaderHandle vertex_shader   = NE_SHADER_HANDLE_NULL;
    NEShaderHandle fragment_shader = NE_SHADER_HANDLE_NULL;

#if defined(_WIN32)

    size_t vert_size = 0, frag_size = 0;
    void *vert_spv = ne_file_read(NE_SPIRV_VERT_PATH, &vert_size);
    void *frag_spv = ne_file_read(NE_SPIRV_FRAG_PATH, &frag_size);
    if (!vert_spv || !frag_spv) {
        NE_LOG_ERROR("failed to load pre-compiled SPIR-V shaders");
        ne_file_free(vert_spv);
        ne_file_free(frag_spv);
        return NE_PIPELINE_HANDLE_NULL;
    }

    vertex_shader = ne_shader_create(renderer, &(NEShaderDesc){
        .stage         = NE_SHADER_STAGE_VERTEX,
        .bytecode      = vert_spv,
        .bytecode_size = vert_size,
        .entry_point   = "main",
    });

    fragment_shader = ne_shader_create(renderer, &(NEShaderDesc){
        .stage         = NE_SHADER_STAGE_FRAGMENT,
        .bytecode      = frag_spv,
        .bytecode_size = frag_size,
        .entry_point   = "main",
    });

    ne_file_free(vert_spv);
    ne_file_free(frag_spv);

#elif defined(__EMSCRIPTEN__) /* Web / WebGPU (WGSL) */

    void *shader_source = ne_file_read("shaders/wgsl/basic.wgsl", NULL);
    if (!shader_source) {
        NE_LOG_ERROR("failed to load shader source: shaders/wgsl/basic.wgsl");
        return NE_PIPELINE_HANDLE_NULL;
    }

    vertex_shader = ne_shader_create_from_source(renderer, &(NEShaderSourceDesc){
        .stage       = NE_SHADER_STAGE_VERTEX,
        .source      = shader_source,
        .entry_point = "vs_main",
        .filename    = "basic.wgsl",
    });

    fragment_shader = ne_shader_create_from_source(renderer, &(NEShaderSourceDesc){
        .stage       = NE_SHADER_STAGE_FRAGMENT,
        .source      = shader_source,
        .entry_point = "fs_main",
        .filename    = "basic.wgsl",
    });

    ne_file_free(shader_source);

#else /* macOS / Metal */

    void *shader_source = ne_file_read("shaders/metal/basic.metal", NULL);
    if (!shader_source) {
        NE_LOG_ERROR("failed to load shader source: shaders/metal/basic.metal");
        return NE_PIPELINE_HANDLE_NULL;
    }

    vertex_shader = ne_shader_create_from_source(renderer, &(NEShaderSourceDesc){
        .stage       = NE_SHADER_STAGE_VERTEX,
        .source      = shader_source,
        .entry_point = "vertexMain",
        .filename    = "basic.metal",
    });

    fragment_shader = ne_shader_create_from_source(renderer, &(NEShaderSourceDesc){
        .stage       = NE_SHADER_STAGE_FRAGMENT,
        .source      = shader_source,
        .entry_point = "fragmentMain",
        .filename    = "basic.metal",
    });

    ne_file_free(shader_source);

#endif /* _WIN32 */

    if (!ne_shader_handle_valid(vertex_shader) || !ne_shader_handle_valid(fragment_shader)) {
        NE_LOG_ERROR("failed to create shaders");
        return NE_PIPELINE_HANDLE_NULL;
    }

    /* ── Create graphics pipeline ──────────────────────────────────────── */

    const NEVertexAttribute vertex_attrs[] = {
        { .location = 0, .format = NE_VERTEX_FORMAT_FLOAT2, .offset = offsetof(Vertex, position) },
        { .location = 1, .format = NE_VERTEX_FORMAT_FLOAT4, .offset = offsetof(Vertex, color) },
    };

    const NEVertexBufferLayout vertex_layout = {
        .stride = sizeof(Vertex),
        .attributes = vertex_attrs,
        .attribute_count = 2,
    };

    NEPipelineHandle pipeline = ne_pipeline_create(renderer, &(NEPipelineDesc){
        .vertex_shader = vertex_shader,
        .fragment_shader = fragment_shader,
        .vertex_layouts = &vertex_layout,
        .vertex_layout_count = 1,
        .topology = NE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    });

    return pipeline;
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
    //----------------------------
    TEST_ALL_IF_TESTING_ENABLED();
    //----------------------------

    ne_logger_set_level(ne_log_get_default_logger(), NE_LOG_LEVEL_TRACE);

    /* ── App + window ──────────────────────────────────────────────────── */

    NEApp *app = ne_app_create();
    if (!app) {
        NE_LOG_ERROR("failed to create app");
        return 1;
    }

    NEWindow *window = ne_window_create(app, &(NEWindowDesc){
                                                 .title = "NanoEngine2 — Triangle",
                                                 .x = 100,
                                                 .y = 100,
                                                 .width = 800,
                                                 .height = 600,
                                                 .resizable = true,
                                                 .transparent = true,
                                                 .undecorated = true});
    if (!window) {
        NE_LOG_ERROR("failed to create window");
        ne_app_destroy(app);
        return 1;
    }

    const NEWindowCallbacks callbacks = {
        .on_key_down = on_key_down,
    };
    ne_window_set_callbacks(window, &callbacks, NULL);

    /* ── Renderer + surface ────────────────────────────────────────────── */
    NERenderer *renderer = ne_renderer_create(app, &(NERendererDesc){.enable_validation = true});
    if (!renderer) {
        NE_LOG_ERROR("failed to create renderer");
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    NERenderSurface *surface = ne_renderer_create_surface(renderer, window,
                                                          &(NERenderSurfaceDesc){
                                                          .vsync = true,
                                                          .clear_color_rgba = {0.0f, 0.0f, 0.0f, 0.0f},
                                                          .present_backend = NE_PRESENT_BACKEND_DXGI});
    if (!surface) {
        NE_LOG_ERROR("failed to create render surface");
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    NEPipelineHandle pipeline = NE_PIPELINE_HANDLE_NULL;
    NEBufferHandle vbo = NE_BUFFER_HANDLE_NULL;
#ifndef __EMSCRIPTEN__
    pipeline = create_basic_pipeline(renderer);
    if (!ne_pipeline_handle_valid(pipeline)) {
        NE_LOG_ERROR("failed to create pipeline");
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    vbo = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_triangle_vertices),
        .usage = NE_BUFFER_USAGE_VERTEX,
        .initial_data = k_triangle_vertices,
        .dynamic = true, /* updated every frame to spin the triangle */
    });
    if (!ne_buffer_handle_valid(vbo)) {
        NE_LOG_ERROR("failed to create vertex buffer");
    }
#endif

    NE_LOG_INFO("triangle demo initialized — press Escape to quit");

    /* ── Frame context ─────────────────────────────────────────────────── */
    /*
     * Single source of truth for what a frame draws. Registered as the window's
     * render dispatch so the platform's frame driver can invoke it.
     *
     * On the web, ne_app_run hands the loop to the browser and main() unwinds
     * while the rAF callback keeps firing — so the context must NOT live on
     * main()'s stack. It is static there. On native it is a plain local.
     */
#ifdef __EMSCRIPTEN__
    static DemoState demo_state;
    static NEFrameContext frame_ctx;
    demo_state = (DemoState){ .angle = 0.0f };
    frame_ctx = (NEFrameContext){
#else
    DemoState demo_state = { .angle = 0.0f };
    NEFrameContext frame_ctx = {
#endif
        .renderer = renderer,
        .surface = surface,
        .pipeline = pipeline,
        .vertex_buffer = vbo,
        .vertex_count = 3,
        .on_update = on_frame_update,
        .user = &demo_state,
    };

    ne_set_window_render_dispatch(window, ne_render_frame, &frame_ctx);

    /* ── Main loop ─────────────────────────────────────────────────────── */
    /*
     * The engine owns the frame loop. ne_app_run drives each window's render
     * dispatch at the platform-correct cadence — a CADisplayLink on macOS
     * (push), the pull loop on Win32, requestAnimationFrame on the web — so
     * application code is identical on every platform.
     *
     * Note: on the web this does not return (the browser owns the loop); the
     * cleanup below runs only on native.
     */
    ne_app_run(app);

    /* Avoid a dangling dispatch pointer into stack memory after the loop. */
    ne_set_window_render_dispatch(window, NULL, NULL);

    /* ── Cleanup ───────────────────────────────────────────────────────── */

    ne_pipeline_destroy(renderer, frame_ctx.pipeline);
    ne_renderer_destroy(renderer);
    ne_window_destroy(window);
    ne_app_destroy(app);

    return 0;
}
