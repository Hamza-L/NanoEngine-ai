#if TESTING_ENABLED

#include "ne_app.h"
#include "ne_log.h"
#include "ne_window.h"

#include "test/ne_test_buffer.h"

[[maybe_unused]] static void TEST_ALL_IF_TESTING_ENABLED() {
    //init
    NEApp *app = ne_app_create();
    if (!app) {
        NE_LOG_ERROR("failed to create app");
        exit(1);
    }

    NEWindow *window = ne_window_create(app, &(NEWindowDesc){
#if TESTING_ENABLED
                                                 .title = "NanoEngine2 — Tests",
#else
                                                 .title = "NanoEngine2",
#endif
                                                 .x = 100,
                                                 .y = 100,
                                                 .width = 800,
                                                 .height = 600,
                                                 .resizable = true});
    if (!window) {
        NE_LOG_ERROR("failed to create window");
        ne_app_destroy(app);
        exit(1);
    }
    /* ── Run Tests ─────────────────────────────────────────────────────── */

    bool test_passed = test_buffer(app, window);

    /* ── Cleanup ───────────────────────────────────────────────────────── */

    ne_window_destroy(window);
    ne_app_destroy(app);

    NE_LOG_INFO("application shutdown complete");
    exit(test_passed ? 0 : 1);
}

#else /* TESTING_ENABLED */

[[maybe_unused]] static void TEST_ALL_IF_TESTING_ENABLED() {
}

#endif /* TESTING_ENABLED */
