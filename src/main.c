#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_test_buffer.h"
#include "ne_window.h"

#include <stdbool.h>

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
                                                 .title = "NanoEngine2 — Tests",
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

    /* ── Run Tests ─────────────────────────────────────────────────────── */

    bool test_passed = test_buffer(app, window);

    /* ── Cleanup ───────────────────────────────────────────────────────── */

    ne_window_destroy(window);
    ne_app_destroy(app);

    NE_LOG_INFO("application shutdown complete");
    return test_passed ? 0 : 1;
}
