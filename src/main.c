#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_window.h"

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

static void on_move(NEWindow *window, int32_t x, int32_t y, void *user_data) {
    (void)window;
    (void)user_data;
    NE_LOG_INFO("move: %d, %d", x, y);
}

static void on_key_down(NEWindow *window, NEKeyEvent event, void *user_data) {
    (void)window;
    (void)user_data;
    NE_LOG_DEBUG("key down: key=%u native=%u mods=%u repeat=%d", (unsigned)event.key, (unsigned)event.native_key_code,
                 (unsigned)event.modifiers, event.repeat ? 1 : 0);
}

static void on_text_input(NEWindow *window, uint32_t codepoint, void *user_data) {
    (void)window;
    (void)user_data;
    NE_LOG_INFO("text input: U+%04X", (unsigned)codepoint);
}

static void on_mouse_move(NEWindow *window, NEMouseMoveEvent event, void *user_data) {
    (void)window;
    (void)user_data;
    NE_LOG_TRACE("mouse move: (%.1f, %.1f) d(%.1f, %.1f)", event.x, event.y, event.delta_x, event.delta_y);
}

static void on_mouse_down(NEWindow *window, NEMouseButtonEvent event, void *user_data) {
    (void)window;
    (void)user_data;
    NE_LOG_TRACE("mouse down: (%.1f, %.1f)", event.x, event.y);
}

int main(void) {
    /* Example: enable verbose logging for the demo application. */
    ne_logger_set_level(ne_log_get_default_logger(), NE_LOG_LEVEL_TRACE);

    NEApp *app = ne_app_create();
    if (!app) {
        NE_LOG_ERROR("failed to create app");
        return 1;
    }

    const NEWindowDesc desc = {
        .title = "NanoEngine - Metal",
        .x = 100,
        .y = 100,
        .width = 1280,
        .height = 720,
        .resizable = true,
    };

    NEWindow *window = ne_window_create(app, &desc);
    if (!window) {
        NE_LOG_ERROR("failed to create window");
        ne_app_destroy(app);
        return 1;
    }

    const NEWindowCallbacks callbacks = {
        .on_close = on_close,
        .on_resize = on_resize,
        .on_move = on_move,
        .on_key_down = on_key_down,
        .on_text_input = on_text_input,
        .on_mouse_move = on_mouse_move,
        .on_mouse_down = on_mouse_down,
    };
    ne_window_set_callbacks(window, &callbacks, NULL);
    ne_window_show(window);

    const NERendererDesc renderer_desc = {
        .backend = NE_RENDER_BACKEND_METAL,
        .enable_validation = true,
    };
    NERenderer *renderer = ne_renderer_create(app, &renderer_desc);
    if (!renderer) {
        NE_LOG_ERROR("failed to create renderer");
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    const NERenderSurfaceDesc surface_desc = {
        .vsync = true,
        .clear_color_rgba = {0.1f, 0.1f, 0.2f, 1.0f},
    };
    NERenderSurface *surface = ne_renderer_create_surface(renderer, window, &surface_desc);
    if (!surface) {
        NE_LOG_ERROR("failed to create render surface");
        ne_renderer_destroy(renderer);
        ne_window_destroy(window);
        ne_app_destroy(app);
        return 1;
    }

    while (ne_window_is_open(window) && ne_app_poll_events(app)) {
        if (ne_renderer_begin_frame(renderer, surface)) {
            ne_renderer_end_frame(renderer, surface);
        }
    }

    ne_renderer_destroy_surface(renderer, surface);
    ne_renderer_destroy(renderer);

    ne_window_destroy(window);
    ne_app_destroy(app);
    return 0;
}
