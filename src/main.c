#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
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
    if (event.key == NE_KEY_ESCAPE) {
        ne_window_request_close(window);
    }
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

    NEWindow *window = ne_window_create(app, &(NEWindowDesc){
                                                 .title = "NanoEngine1",
                                                 .x = 100,
                                                 .y = 100,
                                                 .width = 500,
                                                 .height = 400,
                                                 .resizable = true,
                                                 .undecorated = true,
                                                 .showOnCreate = true});
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

    NERenderer *renderer = ne_renderer_create(app, &(NERendererDesc){.enable_validation = true});
    NERenderSurface *surface = NULL;
    if (renderer) {
        surface = ne_renderer_create_surface(renderer, window, &(NERenderSurfaceDesc){.vsync = true, .clear_color_rgba = {0.1f, 0.1f, 0.2f, 1.0f},});
        if (!surface) {
            NE_LOG_ERROR("failed to create render surface (continuing without renderer)");
            ne_renderer_destroy(renderer);
            renderer = NULL;
        }
    } else {
        NE_LOG_WARN("renderer not available; running window-only loop");
    }

    while (ne_window_is_open(window) && ne_app_poll_events(app)) {
        if (renderer && surface) {
            if (ne_renderer_begin_frame(renderer, surface)) {
                ne_renderer_end_frame(renderer, surface);
            } else {
                /* Avoid burning CPU if the surface isn't ready yet (minimized, etc.). */
                NE_PLATFORM_YIELD_MS_1();
            }
        } else {
            NE_PLATFORM_YIELD_MS_1();
        }
    }

    if (renderer && surface) {
        ne_renderer_destroy_surface(renderer, surface);
    }
    if (renderer) {
        ne_renderer_destroy(renderer);
    }

    ne_window_destroy(window);
    ne_app_destroy(app);
    return 0;
}
