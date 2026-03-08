#ifndef NE_WINDOW_H
#define NE_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to the platform application instance. */
typedef struct NEApp NEApp;

/**
 * Opaque handle to a platform window.
 *
 * A window is created with `ne_window_create` and destroyed with
 * `ne_window_destroy`.
 */
typedef struct NEWindow NEWindow;

/**
 * Platform-native window handle types.
 *
 * These are intended for integrations (render backends, native UI embedding).
 * Callers must only request handle types that make sense for the current
 * platform/build.
 */
typedef enum NENativeHandleType {
    /** macOS: `NSWindow*` */
    NE_NATIVE_HANDLE_COCOA_NS_WINDOW = 1,

    /** macOS: `NSView*` (window content view) */
    NE_NATIVE_HANDLE_COCOA_NS_VIEW = 2,

    /** Windows: `HWND` (unsupported on macOS builds; will assert if requested) */
    NE_NATIVE_HANDLE_WIN32_HWND = 100,

    /** X11: `Window` (unsupported on macOS builds; will assert if requested) */
    NE_NATIVE_HANDLE_X11_WINDOW = 200,

    /** Wayland: `wl_surface*` (unsupported on macOS builds; will assert if requested) */
    NE_NATIVE_HANDLE_WAYLAND_SURFACE = 300,
} NENativeHandleType;

/**
 * Window creation parameters.
 */
typedef struct NEWindowDesc {
    /** UTF-8 title displayed by the OS (may be NULL for a default title). */
    const char *title;

    /** Initial x position in screen coordinates (platform-dependent). */
    int32_t x;

    /** Initial y position in screen coordinates (platform-dependent). */
    int32_t y;

    /** Initial client width in pixels. Must be > 0. */
    int32_t width;

    /** Initial client height in pixels. Must be > 0. */
    int32_t height;

    /** Whether the user can resize the window. */
    bool resizable;

    /** Whether the window should have no titlebar or border (borderless). */
    bool undecorated;

    /** Whether the window is shown immediately after creation. */
    bool showOnCreate;
} NEWindowDesc;

/**
 * Modifier key bit flags.
 *
 * These flags are intended to be platform-normalized.
 */
typedef uint32_t NEModifiers;

enum {
    NE_MOD_SHIFT = 1u << 0,
    NE_MOD_CONTROL = 1u << 1,
    NE_MOD_ALT = 1u << 2,
    NE_MOD_SUPER = 1u << 3,
    NE_MOD_CAPS_LOCK = 1u << 4,
};

/**
 * Platform-normalized key identifiers.
 *
 * Notes:
 * - Not all keys are mapped on all platforms yet.
 * - Use `native_key_code` in `NEKeyEvent` if you need platform-specific codes.
 */
typedef enum NEKey {
    NE_KEY_UNKNOWN = 0,

    /* Letters */
    NE_KEY_A,
    NE_KEY_B,
    NE_KEY_C,
    NE_KEY_D,
    NE_KEY_E,
    NE_KEY_F,
    NE_KEY_G,
    NE_KEY_H,
    NE_KEY_I,
    NE_KEY_J,
    NE_KEY_K,
    NE_KEY_L,
    NE_KEY_M,
    NE_KEY_N,
    NE_KEY_O,
    NE_KEY_P,
    NE_KEY_Q,
    NE_KEY_R,
    NE_KEY_S,
    NE_KEY_T,
    NE_KEY_U,
    NE_KEY_V,
    NE_KEY_W,
    NE_KEY_X,
    NE_KEY_Y,
    NE_KEY_Z,

    /* Digits */
    NE_KEY_0,
    NE_KEY_1,
    NE_KEY_2,
    NE_KEY_3,
    NE_KEY_4,
    NE_KEY_5,
    NE_KEY_6,
    NE_KEY_7,
    NE_KEY_8,
    NE_KEY_9,

    /* Controls */
    NE_KEY_ESCAPE,
    NE_KEY_ENTER,
    NE_KEY_TAB,
    NE_KEY_BACKSPACE,
    NE_KEY_SPACE,

    /* Arrows */
    NE_KEY_LEFT,
    NE_KEY_RIGHT,
    NE_KEY_UP,
    NE_KEY_DOWN,
} NEKey;

/**
 * Mouse button identifiers.
 */
typedef enum NEMouseButton {
    NE_MOUSE_BUTTON_LEFT = 0,
    NE_MOUSE_BUTTON_RIGHT = 1,
    NE_MOUSE_BUTTON_MIDDLE = 2,
    NE_MOUSE_BUTTON_OTHER = 3,
} NEMouseButton;

/**
 * Keyboard input event.
 */
typedef struct NEKeyEvent {
    /** Platform-normalized key identifier. */
    NEKey key;

    /** Platform-native key code (useful for debugging/missing mappings). */
    uint16_t native_key_code;

    /** Bitwise OR of `NEModifiers` values. */
    NEModifiers modifiers;

    /** True if the OS generated this event as an auto-repeat. */
    bool repeat;
} NEKeyEvent;

/**
 * Mouse move event, in window client coordinates.
 */
typedef struct NEMouseMoveEvent {
    /** Current x position in client coordinates. */
    float x;

    /** Current y position in client coordinates. */
    float y;

    /** Delta in x since last mouse move event (best-effort). */
    float delta_x;

    /** Delta in y since last mouse move event (best-effort). */
    float delta_y;
} NEMouseMoveEvent;

/**
 * Mouse button press/release event.
 */
typedef struct NEMouseButtonEvent {
    /** Which mouse button generated the event. */
    NEMouseButton button;

    /** Current x position in client coordinates. */
    float x;

    /** Current y position in client coordinates. */
    float y;

    /** Bitwise OR of `NEModifiers` values. */
    NEModifiers modifiers;
} NEMouseButtonEvent;

/**
 * Mouse scroll event.
 */
typedef struct NEMouseScrollEvent {
    /** Scroll delta in x (units are platform-dependent). */
    float delta_x;

    /** Scroll delta in y (units are platform-dependent). */
    float delta_y;
} NEMouseScrollEvent;

/**
 * Window close callback.
 *
 * Called when the OS is closing the window (e.g. user pressed the close button
 * or the program requested closing). This callback is notification-only and is
 * not cancellable.
 */
typedef void (*NEOnCloseFn)(NEWindow *window, void *user_data);

/** Called after the window's client area size changed. */
typedef void (*NEOnResizeFn)(NEWindow *window, int32_t width, int32_t height, void *user_data);

/** Called after the window moved on screen (coordinates are platform-dependent). */
typedef void (*NEOnMoveFn)(NEWindow *window, int32_t x, int32_t y, void *user_data);

/** Called when a key is pressed or released. */
typedef void (*NEOnKeyFn)(NEWindow *window, NEKeyEvent event, void *user_data);

/**
 * Unicode text input callback.
 *
 * This callback is intended for *text entry* (UI, console, editor fields).
 * The `codepoint` represents the Unicode scalar value produced by the active
 * keyboard layout / input method.
 *
 * Notes:
 * - This is separate from raw key events (`on_key_down` / `on_key_up`), which
 *   are typically used for gameplay and shortcuts.
 * - The engine may emit multiple callbacks for a single OS event (e.g. when a
 *   composed character results in multiple Unicode codepoints).
 */
typedef void (*NEOnTextInputFn)(NEWindow *window, uint32_t codepoint, void *user_data);

/** Called when the mouse moves within the window. */
typedef void (*NEOnMouseMoveFn)(NEWindow *window, NEMouseMoveEvent event, void *user_data);

/** Called when a mouse button is pressed or released. */
typedef void (*NEOnMouseButtonFn)(NEWindow *window, NEMouseButtonEvent event, void *user_data);

/** Called when the mouse wheel/trackpad scrolls. */
typedef void (*NEOnMouseScrollFn)(NEWindow *window, NEMouseScrollEvent event, void *user_data);

/**
 * Set of optional window callbacks.
 *
 * Any callback may be NULL.
 */
typedef struct NEWindowCallbacks {
    NEOnCloseFn on_close;
    NEOnResizeFn on_resize;
    NEOnMoveFn on_move;

    /** Raw key press events (physical key). */
    NEOnKeyFn on_key_down;

    /** Raw key release events (physical key). */
    NEOnKeyFn on_key_up;

    /** Text input events (Unicode characters). */
    NEOnTextInputFn on_text_input;

    NEOnMouseMoveFn on_mouse_move;
    NEOnMouseButtonFn on_mouse_down;
    NEOnMouseButtonFn on_mouse_up;
    NEOnMouseScrollFn on_mouse_scroll;
} NEWindowCallbacks;

/**
 * Create a new window.
 *
 * Parameters:
 * - `app`: Application instance returned by `ne_app_create`.
 * - `desc`: Window creation parameters.
 *
 * Returns:
 * - A valid `NEWindow*` on success.
 * - `NULL` on failure.
 */
NEWindow *ne_window_create(NEApp *app, const NEWindowDesc *desc);

/**
 * Destroy a window and release its resources.
 *
 * This function does not call user callbacks while tearing down the window.
 *
 * Parameters:
 * - `window`: Window instance (may be NULL).
 */
void ne_window_destroy(NEWindow *window);

/**
 * Show a window.
 *
 * Parameters:
 * - `window`: Window instance.
 */
void ne_window_show(NEWindow *window);

/**
 * Hide a window.
 *
 * Parameters:
 * - `window`: Window instance.
 */
void ne_window_hide(NEWindow *window);

/**
 * Request that the OS closes the window.
 *
 * The actual close may occur asynchronously as the OS processes events.
 *
 * Parameters:
 * - `window`: Window instance.
 */
void ne_window_request_close(NEWindow *window);

/**
 * Set the window title.
 *
 * Parameters:
 * - `window`: Window instance.
 * - `title`: UTF-8 title string (must not be NULL).
 */
void ne_window_set_title(NEWindow *window, const char *title);

/**
 * Set the window position (platform-dependent screen coordinates).
 *
 * Parameters:
 * - `window`: Window instance.
 * - `x`: New x position.
 * - `y`: New y position.
 */
void ne_window_set_position(NEWindow *window, int32_t x, int32_t y);

/**
 * Set the window size.
 *
 * Parameters:
 * - `window`: Window instance.
 * - `width`: New width in pixels (must be > 0).
 * - `height`: New height in pixels (must be > 0).
 */
void ne_window_set_size(NEWindow *window, int32_t width, int32_t height);

/**
 * Set or clear window callbacks.
 *
 * Parameters:
 * - `window`: Window instance.
 * - `callbacks`: Pointer to callbacks. If NULL, all callbacks are cleared.
 * - `user_data`: Pointer passed back to every callback invocation.
 */
void ne_window_set_callbacks(NEWindow *window, const NEWindowCallbacks *callbacks, void *user_data);

/**
 * Get the window content (client) size in logical units.
 *
 * This is the size of the drawable content region excluding window
 * decorations. Units are platform-dependent logical units (points on macOS).
 *
 * Parameters:
 * - `window`: Window instance.
 * - `out_width`: Receives width (may be NULL).
 * - `out_height`: Receives height (may be NULL).
 *
 * Returns:
 * - `true` on success.
 * - `false` if `window` is NULL/closed or the value cannot be queried.
 */
bool ne_window_get_content_size(const NEWindow *window, int32_t *out_width, int32_t *out_height);

/**
 * Get the framebuffer size in physical pixels.
 *
 * This is the size the renderer should use for swapchain/backbuffer
 * configuration. On high-DPI displays this may be larger than the content size.
 *
 * Parameters:
 * - `window`: Window instance.
 * - `out_width`: Receives pixel width (may be NULL).
 * - `out_height`: Receives pixel height (may be NULL).
 *
 * Returns:
 * - `true` on success.
 * - `false` if `window` is NULL/closed or the value cannot be queried.
 */
bool ne_window_get_framebuffer_size(const NEWindow *window, int32_t *out_width, int32_t *out_height);

/**
 * Get the content-to-framebuffer scale factor.
 *
 * This is typically `framebuffer_pixels / content_logical_units`.
 * Example: 2.0 on macOS Retina at default scaling.
 *
 * Parameters:
 * - `window`: Window instance.
 * - `out_scale`: Receives the scale factor.
 *
 * Returns:
 * - `true` on success.
 * - `false` if `window` is NULL/closed or the value cannot be queried.
 */
bool ne_window_get_content_scale(const NEWindow *window, float *out_scale);

/**
 * Get a platform-native handle for the window.
 *
 * Returns:
 * - A non-NULL platform handle on success.
 * - NULL if `window` is NULL/closed or the handle cannot be retrieved.
 *
 * Asserts:
 * - If `type` is not supported on the current platform/build.
 *
 * Notes:
 * - The returned pointer is owned by the window; do not free/release it.
 * - The concrete type depends on `type` (e.g., `NSView*`, `NSWindow*` on macOS).
 */
void *ne_window_get_native_handle(NEWindow *window, NENativeHandleType type);

/**
 * Query whether a mouse button is currently held down.
 *
 * This is based on the latest input events processed by `ne_app_poll_events`.
 *
 * Parameters:
 * - `window`: Window instance.
 * - `button`: The mouse button to query.
 *
 * Returns:
 * - `true` if the button is currently down, otherwise `false`.
 */
bool ne_window_is_mouse_button_down(const NEWindow *window, NEMouseButton button);

/**
 * Query whether a window is currently open.
 *
 * Parameters:
 * - `window`: Window instance.
 *
 * Returns:
 * - `true` if the window exists and is open, otherwise `false`.
 */
bool ne_window_is_open(const NEWindow *window);

#ifdef __cplusplus
}
#endif

#endif
