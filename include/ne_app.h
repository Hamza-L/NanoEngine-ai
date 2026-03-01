#ifndef NE_APP_H
#define NE_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle to the platform application instance.
 *
 * The application owns the event loop / event pump. On some platforms (e.g.
 * macOS) there is effectively a single process-wide application instance.
 */
typedef struct NEApp NEApp;

/**
 * Create (or acquire) the application instance.
 *
 * Call this once before creating windows and before calling `ne_app_poll_events`
 * or `ne_app_run`.
 *
 * Usage:
 * - `NEApp *app = ne_app_create();`
 * - Create one or more windows using `ne_window_create(app, ...)`
 * - Pump events via `ne_app_poll_events(app)` or `ne_app_run(app)`
 * - Release via `ne_app_destroy(app)`
 *
 * Returns:
 * - A valid `NEApp*` on success.
 * - `NULL` on failure.
 */
NEApp *ne_app_create(void);

/**
 * Release (and possibly destroy) the application instance.
 *
 * If the application is shared internally (e.g. via a reference count), the
 * underlying platform resources are freed only when the last reference is
 * released.
 *
 * Parameters:
 * - `app`: The application instance returned by `ne_app_create` (may be NULL).
 */
void ne_app_destroy(NEApp *app);

/**
 * Pump pending OS events once.
 *
 * Typical usage is to call this function once per frame. It processes pending
 * input/window events and updates internal state.
 *
 * Parameters:
 * - `app`: Application instance.
 *
 * Returns:
 * - `true` if the application is still running.
 * - `false` if the application should exit (e.g. quit requested and/or no more
 *   windows).
 */
bool ne_app_poll_events(NEApp *app);

/**
 * Run a simple event loop until the application stops.
 *
 * This is a convenience wrapper around repeated calls to
 * `ne_app_poll_events(app)`.
 *
 * Parameters:
 * - `app`: Application instance.
 */
void ne_app_run(NEApp *app);

/**
 * Request that the application stops running.
 *
 * Parameters:
 * - `app`: Application instance.
 */
void ne_app_request_quit(NEApp *app);

/**
 * Query whether the application is currently running.
 *
 * Parameters:
 * - `app`: Application instance.
 *
 * Returns:
 * - `true` if running, otherwise `false`.
 */
bool ne_app_is_running(const NEApp *app);

/**
 * Get the number of currently open windows created by this application.
 *
 * Parameters:
 * - `app`: Application instance.
 *
 * Returns:
 * - The number of open windows.
 */
uint32_t ne_app_get_window_count(const NEApp *app);

#ifdef __cplusplus
}
#endif

#endif
