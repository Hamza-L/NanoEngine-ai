#include "ne_app.h"
#include "ne_file.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_window.h"

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
    NE_LOG_INFO("resize: %d x %d", width, height);
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
    float color[3];
} Vertex;

static const Vertex k_triangle_vertices[] = {
    {{ 0.0f,  0.5f}, {1.0f, 0.0f, 0.0f}},  /* top    — red   */
    {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},  /* left   — green */
    {{ 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},  /* right  — blue  */
};

static const uint16_t k_triangle_indices[] = {0, 1, 2};

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
    ne_logger_set_level(ne_log_get_default_logger(), NE_LOG_LEVEL_TRACE);

    /* ── App + window ──────────────────────────────────────────────────── */

    NEApp *app = ne_app_create();
    if (!app) {
        NE_LOG_ERROR("failed to create app");
        return 1;
    }

    NEWindow *window = ne_window_create(app, &(NEWindowDesc){
                                                 .title = "NanoEngine2 — Buffer Test",
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

    /* ── Test GPU Buffer Creation ──────────────────────────────────────── */

    NE_LOG_INFO("testing GPU buffer creation...");

    /* Create vertex buffer with initial data */
    NEBufferHandle vbo = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_triangle_vertices),
        .usage = NE_BUFFER_USAGE_VERTEX,
        .initial_data = k_triangle_vertices,
    });

    if (!ne_buffer_handle_valid(vbo)) {
        NE_LOG_ERROR("failed to create vertex buffer");
        ne_renderer_destroy_surface(renderer, surface);
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }
    NE_LOG_INFO("✓ vertex buffer created (handle id=%u, size=%zu bytes)", vbo.id, sizeof(k_triangle_vertices));

    /* Create index buffer */
    NEBufferHandle ibo = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_triangle_indices),
        .usage = NE_BUFFER_USAGE_INDEX,
        .initial_data = k_triangle_indices,
    });

    if (!ne_buffer_handle_valid(ibo)) {
        NE_LOG_ERROR("failed to create index buffer");
        ne_renderer_destroy_surface(renderer, surface);
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }
    NE_LOG_INFO("✓ index buffer created (handle id=%u, size=%zu bytes)", ibo.id, sizeof(k_triangle_indices));

    /* Test buffer update */
    Vertex updated_vertices[] = {
        {{ 0.2f,  0.5f}, {1.0f, 0.5f, 0.0f}},
        {{-0.5f, -0.3f}, {0.5f, 1.0f, 0.0f}},
        {{ 0.5f, -0.3f}, {0.0f, 0.5f, 1.0f}},
    };

    ne_buffer_update(renderer, vbo, updated_vertices, sizeof(updated_vertices), 0);
    NE_LOG_INFO("✓ vertex buffer updated successfully");

    NE_LOG_INFO("buffer implementation test completed successfully!");
    NE_LOG_INFO("press Escape to quit or window will auto-close in 3 seconds");

    /* ── Main loop (simple clear loop until escape or timeout) ─────────────── */

    int frame_count = 0;
    const int max_frames = 180;  /* ~3 seconds at 60 FPS */

    while (ne_window_is_open(window) && ne_app_poll_events(app) && frame_count < max_frames) {
        NERenderPass *pass = ne_renderer_begin_frame(renderer, surface);
        if (pass) {
            /* For now, just clear the screen. Actual rendering comes later. */
            ne_renderer_end_frame(renderer, pass);
        } else {
            NE_PLATFORM_YIELD_MS_1();
        }
        frame_count++;
    }

    NE_LOG_INFO("test completed after %d frames", frame_count);

    /* ── Cleanup ───────────────────────────────────────────────────────── */

    ne_buffer_destroy(renderer, ibo);
    ne_buffer_destroy(renderer, vbo);
    ne_renderer_destroy_surface(renderer, surface);
    ne_renderer_destroy(renderer);
    ne_window_destroy(window);
    ne_app_destroy(app);

    NE_LOG_INFO("application shutdown complete");
    return 0;
}
