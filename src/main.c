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

/*
 * Cross-platform tiny yield used only in the "no renderer" fallback loop.
 * On macOS this is a no-op to keep behavior identical to the existing demo.
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NE_PLATFORM_YIELD_MS_1() Sleep(1)
#else
#define NE_PLATFORM_YIELD_MS_1() ((void)0)
#endif

/* ── Callbacks ──────────────────────────────────────────────────────────── */

static void on_close(NEWindow *window, void *user_data) {
    (void)window;
    (void)user_data;
    NE_LOG_INFO("window closing");
}

static void on_resize(NEWindow *window, int32_t width, int32_t height, void *user_data) {
    (void)window;
    (void)user_data;
    (void)width;
    (void)height;
    // NE_LOG_INFO("resize: %d x %d", width, height);
}

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

static void on_frame_update(NEFrameContext *ctx, NERenderPass *pass) {
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
                                                 .show_on_create = true});
    if (!window) {
        NE_LOG_ERROR("failed to create window");
        ne_app_destroy(app);
        return 1;
    }

    const NEWindowCallbacks callbacks = {
        .on_close = on_close,
        .on_resize = on_resize,
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

    NERenderSurface *surface = ne_renderer_create_surface(renderer,
                                                          window,
                                                          &(NERenderSurfaceDesc){.vsync = true, .clear_color_rgba = {0.1f, 0.1f, 0.12f, 1.0f}});
    if (!surface) {
        NE_LOG_ERROR("failed to create render surface");
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    NEShaderHandle vertex_shader   = NE_SHADER_HANDLE_NULL;
    NEShaderHandle fragment_shader = NE_SHADER_HANDLE_NULL;

#if defined(_WIN32)

    vertex_shader = ne_shader_create_from_source(renderer, &(NEShaderSourceDesc){
        .stage         = NE_SHADER_STAGE_VERTEX,
        .filename      = "shaders/glsl/basic.vert",
        .entry_point   = "main",
    });

    fragment_shader = ne_shader_create_from_source(renderer, &(NEShaderSourceDesc){
        .stage         = NE_SHADER_STAGE_FRAGMENT,
        .filename      = "shaders/glsl/basic.frag",
        .entry_point   = "main",
    });

#else /* macOS / Metal */

    void *shader_source = ne_file_read("shaders/metal/basic.metal", NULL);
    if (!shader_source) {
        NE_LOG_ERROR("failed to load shader source: shaders/metal/basic.metal");
        ne_renderer_destroy_surface(renderer, surface);
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
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
        ne_renderer_destroy_surface(renderer, surface);
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    /* ── Create vertex buffer ──────────────────────────────────────────── */

    NEBufferHandle vbo = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_triangle_vertices),
        .usage = NE_BUFFER_USAGE_VERTEX,
        .initial_data = k_triangle_vertices,
        .dynamic = true, /* updated every frame to spin the triangle */
    });

    if (!ne_buffer_handle_valid(vbo)) {
        NE_LOG_ERROR("failed to create vertex buffer");
        ne_renderer_destroy_surface(renderer, surface);
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
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

    if (!ne_pipeline_handle_valid(pipeline)) {
        NE_LOG_ERROR("failed to create pipeline");
        ne_renderer_destroy_surface(renderer, surface);
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    NE_LOG_INFO("triangle demo initialized — press Escape to quit");

    /* ── Frame context ─────────────────────────────────────────────────── */
    /* Single source of truth for what a frame draws. It is registered as the
     * window's render dispatch so the platform's frame driver can invoke it. */
    DemoState demo_state = { .angle = 0.0f };
    NEFrameContext frame_ctx = {
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
     * Frame-driving model differs by platform:
     *  - macOS (push): a CADisplayLink drives ne_render_frame at the display
     *    refresh (including during a window resize). The main loop only pumps
     *    events and blocks between them, so it must NOT render here.
     *  - Win32 (pull): the app owns the loop and renders each iteration.
     */
#if defined(_WIN32)
    while (ne_window_is_open(window) && ne_app_poll_events(app)) {
        ne_render_frame(&frame_ctx);
    }
#else
    while (ne_window_is_open(window) && ne_app_poll_events(app)) {
        /* Rendering is driven by the platform frame driver (display link). */
    }
#endif

    /* Avoid a dangling dispatch pointer into stack memory after the loop. */
    ne_set_window_render_dispatch(window, NULL, NULL);

    /* ── Cleanup ───────────────────────────────────────────────────────── */

    ne_pipeline_destroy(renderer, pipeline);
    ne_buffer_destroy(renderer, vbo);
    ne_shader_destroy(renderer, fragment_shader);
    ne_shader_destroy(renderer, vertex_shader);
    ne_renderer_destroy_surface(renderer, surface);
    ne_renderer_destroy(renderer);
    ne_window_destroy(window);
    ne_app_destroy(app);

    return 0;
}
