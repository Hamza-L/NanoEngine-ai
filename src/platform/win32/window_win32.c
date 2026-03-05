#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h> /* GET_X_LPARAM / GET_Y_LPARAM */

#include "ne_app.h"
#include "ne_log.h"
#include "ne_window.h"

#include <stdlib.h>
#include <string.h>

struct NEApp {
    HINSTANCE hinstance;
    ATOM window_class;

    uint32_t window_count;

    bool initialized;
    bool running;
};

static struct {
    NEApp *app;
    uint32_t refcount;
} g_app_state = {0};

struct NEWindow {
    NEApp *app;
    HWND hwnd;

    NEWindowCallbacks callbacks;
    void *user_data;

    bool open;
    bool destroying;

    uint32_t mouse_buttons_down_mask;

    float last_mouse_x;
    float last_mouse_y;
    bool has_last_mouse;

    /* UTF-16 surrogate tracking for WM_CHAR text input. */
    uint16_t pending_high_surrogate;
};

static wchar_t *ne_utf8_to_wide(const char *utf8) {
    if (!utf8) {
        return NULL;
    }

    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }

    wchar_t *w = (wchar_t *)calloc((size_t)needed, sizeof(wchar_t));
    if (!w) {
        return NULL;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, needed) <= 0) {
        free(w);
        return NULL;
    }

    return w;
}

static NEModifiers ne_get_modifiers(void) {
    NEModifiers mods = 0;

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        mods |= NE_MOD_SHIFT;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        mods |= NE_MOD_CONTROL;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        mods |= NE_MOD_ALT;
    }
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        mods |= NE_MOD_SUPER;
    }
    if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0) {
        mods |= NE_MOD_CAPS_LOCK;
    }

    return mods;
}

static NEKey ne_map_key(WPARAM vk) {
    /* Letters/digits map directly for WM_KEYDOWN virtual key codes. */
    if (vk >= 'A' && vk <= 'Z') {
        return (NEKey)(NE_KEY_A + (uint32_t)(vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return (NEKey)(NE_KEY_0 + (uint32_t)(vk - '0'));
    }

    switch (vk) {
    case VK_ESCAPE:
        return NE_KEY_ESCAPE;
    case VK_RETURN:
        return NE_KEY_ENTER;
    case VK_TAB:
        return NE_KEY_TAB;
    case VK_BACK:
        return NE_KEY_BACKSPACE;
    case VK_SPACE:
        return NE_KEY_SPACE;

    case VK_LEFT:
        return NE_KEY_LEFT;
    case VK_RIGHT:
        return NE_KEY_RIGHT;
    case VK_UP:
        return NE_KEY_UP;
    case VK_DOWN:
        return NE_KEY_DOWN;

    default:
        return NE_KEY_UNKNOWN;
    }
}

static NEMouseButton ne_map_mouse_button(UINT msg, WPARAM wparam) {
    (void)wparam;

    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return NE_MOUSE_BUTTON_LEFT;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return NE_MOUSE_BUTTON_RIGHT;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return NE_MOUSE_BUTTON_MIDDLE;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
        const WORD xbtn = GET_XBUTTON_WPARAM(wparam);
        return (xbtn == XBUTTON1 || xbtn == XBUTTON2) ? NE_MOUSE_BUTTON_OTHER : NE_MOUSE_BUTTON_OTHER;
    }
    default:
        return NE_MOUSE_BUTTON_OTHER;
    }
}

static void ne_window_update_mouse_button_mask(NEWindow *window, NEMouseButton button, bool down) {
    if (!window) {
        return;
    }
    if ((uint32_t)button >= 32u) {
        return;
    }

    const uint32_t bit = 1u << (uint32_t)button;
    if (down) {
        window->mouse_buttons_down_mask |= bit;
    } else {
        window->mouse_buttons_down_mask &= ~bit;
    }
}

static void ne_emit_text_input_codepoint(NEWindow *window, uint32_t codepoint) {
    if (!window || !window->callbacks.on_text_input) {
        return;
    }
    window->callbacks.on_text_input(window, codepoint, window->user_data);
}

static LRESULT CALLBACK ne_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    NEWindow *window = (NEWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lparam;
        NEWindow *w = (NEWindow *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
        if (w) {
            w->hwnd = hwnd;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    case WM_CLOSE:
        /* Always allow closing. Actual teardown/callback happens in WM_DESTROY. */
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (window && window->open) {
            window->open = false;
            if (window->app && window->app->window_count > 0) {
                window->app->window_count--;
            }

            /* Avoid firing user callbacks during programmatic teardown. */
            if (!window->destroying && window->callbacks.on_close) {
                window->callbacks.on_close(window, window->user_data);
            }
        }
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)0);
        return DefWindowProcW(hwnd, msg, wparam, lparam);

    case WM_SIZE:
        if (window && window->callbacks.on_resize) {
            const int32_t w = (int32_t)LOWORD(lparam);
            const int32_t h = (int32_t)HIWORD(lparam);
            window->callbacks.on_resize(window, w, h, window->user_data);
        }
        return 0;

    case WM_MOVE:
        if (window && window->callbacks.on_move) {
            const int32_t x = (int32_t)(int16_t)LOWORD(lparam);
            const int32_t y = (int32_t)(int16_t)HIWORD(lparam);
            window->callbacks.on_move(window, x, y, window->user_data);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (window && window->callbacks.on_key_down) {
            const bool repeat = (lparam & (1l << 30)) != 0;
            const uint16_t scancode = (uint16_t)((lparam >> 16) & 0xFF);
            const NEKeyEvent e = {
                .key = ne_map_key(wparam),
                .native_key_code = scancode,
                .modifiers = ne_get_modifiers(),
                .repeat = repeat,
            };
            window->callbacks.on_key_down(window, e, window->user_data);
        }
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (window && window->callbacks.on_key_up) {
            const uint16_t scancode = (uint16_t)((lparam >> 16) & 0xFF);
            const NEKeyEvent e = {
                .key = ne_map_key(wparam),
                .native_key_code = scancode,
                .modifiers = ne_get_modifiers(),
                .repeat = false,
            };
            window->callbacks.on_key_up(window, e, window->user_data);
        }
        return 0;

    case WM_CHAR:
        if (window) {
            const uint16_t c = (uint16_t)wparam;
            if (c >= 0xD800 && c <= 0xDBFF) {
                /* high surrogate */
                window->pending_high_surrogate = c;
            } else {
                uint32_t codepoint = (uint32_t)c;
                if (c >= 0xDC00 && c <= 0xDFFF && window->pending_high_surrogate) {
                    const uint32_t high = (uint32_t)window->pending_high_surrogate;
                    const uint32_t low = (uint32_t)c;
                    codepoint = 0x10000u + (((high - 0xD800u) << 10) | (low - 0xDC00u));
                }
                window->pending_high_surrogate = 0;
                ne_emit_text_input_codepoint(window, codepoint);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (window && window->callbacks.on_mouse_move) {
            const float x = (float)GET_X_LPARAM(lparam);
            const float y = (float)GET_Y_LPARAM(lparam);

            const NEMouseMoveEvent e = {
                .x = x,
                .y = y,
                .delta_x = window->has_last_mouse ? (x - window->last_mouse_x) : 0.0f,
                .delta_y = window->has_last_mouse ? (y - window->last_mouse_y) : 0.0f,
            };

            window->last_mouse_x = x;
            window->last_mouse_y = y;
            window->has_last_mouse = true;

            window->callbacks.on_mouse_move(window, e, window->user_data);
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (window) {
            const NEMouseButton button = ne_map_mouse_button(msg, wparam);
            ne_window_update_mouse_button_mask(window, button, true);

            if (window->callbacks.on_mouse_down) {
                const NEMouseButtonEvent e = {
                    .button = button,
                    .x = (float)GET_X_LPARAM(lparam),
                    .y = (float)GET_Y_LPARAM(lparam),
                    .modifiers = ne_get_modifiers(),
                };
                window->callbacks.on_mouse_down(window, e, window->user_data);
            }
        }
        return (msg == WM_XBUTTONDOWN) ? TRUE : 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        if (window) {
            const NEMouseButton button = ne_map_mouse_button(msg, wparam);
            ne_window_update_mouse_button_mask(window, button, false);

            if (window->callbacks.on_mouse_up) {
                const NEMouseButtonEvent e = {
                    .button = button,
                    .x = (float)GET_X_LPARAM(lparam),
                    .y = (float)GET_Y_LPARAM(lparam),
                    .modifiers = ne_get_modifiers(),
                };
                window->callbacks.on_mouse_up(window, e, window->user_data);
            }
        }
        return (msg == WM_XBUTTONUP) ? TRUE : 0;

    case WM_MOUSEWHEEL:
        if (window && window->callbacks.on_mouse_scroll) {
            /* WHEEL_DELTA is 120 units per notch. Keep units platform-dependent for now. */
            const float dy = (float)GET_WHEEL_DELTA_WPARAM(wparam);
            const NEMouseScrollEvent e = {
                .delta_x = 0.0f,
                .delta_y = dy,
            };
            window->callbacks.on_mouse_scroll(window, e, window->user_data);
        }
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

static bool ne_register_window_class(NEApp *app) {
    if (!app) {
        return false;
    }
    if (app->window_class) {
        return true;
    }

    const wchar_t *class_name = L"NanoEngineWindow";

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = ne_wndproc;
    wc.hInstance = app->hinstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;

    const ATOM atom = RegisterClassExW(&wc);
    if (!atom) {
        const DWORD err = GetLastError();
        NE_LOG_ERROR("RegisterClassExW failed (err=%lu)", (unsigned long)err);
        return false;
    }

    app->window_class = atom;
    return true;
}

NEApp *ne_app_create(void) {
    if (g_app_state.app) {
        g_app_state.refcount++;
        return g_app_state.app;
    }

    NEApp *app = (NEApp *)calloc(1, sizeof(NEApp));
    if (!app) {
        return NULL;
    }

    app->hinstance = GetModuleHandleW(NULL);
    app->initialized = true;
    app->running = true;
    app->window_count = 0;

    if (!ne_register_window_class(app)) {
        free(app);
        return NULL;
    }

    g_app_state.app = app;
    g_app_state.refcount = 1;
    return app;
}

void ne_app_destroy(NEApp *app) {
    if (!app || app != g_app_state.app || g_app_state.refcount == 0) {
        return;
    }

    g_app_state.refcount--;
    if (g_app_state.refcount > 0) {
        return;
    }

    app->running = false;
    app->initialized = false;
    app->window_count = 0;

    if (app->window_class) {
        const wchar_t *class_name = L"NanoEngineWindow";
        UnregisterClassW(class_name, app->hinstance);
        app->window_class = 0;
    }

    free(app);
    g_app_state.app = NULL;
}

bool ne_app_poll_events(NEApp *app) {
    if (!app || !app->initialized || !app->running) {
        return false;
    }

    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            app->running = false;
            break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (app->window_count == 0) {
        app->running = false;
    }

    return app->running;
}

void ne_app_run(NEApp *app) {
    /*
     * This is a convenience loop only.
     * `ne_app_poll_events` is intentionally non-blocking (so a game loop can
     * call it once per frame). Without a small sleep here, `ne_app_run` would
     * busy-spin at 100% CPU when there are no messages.
     */
    while (ne_app_poll_events(app)) {
        Sleep(1);
    }
}

void ne_app_request_quit(NEApp *app) {
    if (!app) {
        return;
    }
    app->running = false;
}

bool ne_app_is_running(const NEApp *app) {
    return app && app->running;
}

uint32_t ne_app_get_window_count(const NEApp *app) {
    return app ? app->window_count : 0;
}

NEWindow *ne_window_create(NEApp *app, const NEWindowDesc *desc) {
    if (!app || !app->initialized || !app->running || !desc || desc->width <= 0 || desc->height <= 0) {
        return NULL;
    }

    if (!ne_register_window_class(app)) {
        return NULL;
    }

    NEWindow *window = (NEWindow *)calloc(1, sizeof(NEWindow));
    if (!window) {
        return NULL;
    }

    window->app = app;
    window->open = true;
    window->destroying = false;

    const DWORD style_base = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD style = style_base;
    if (desc->resizable) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    }

    // adjusts the size of the window by adding the size of decorators and border so the client area is the actual area given
    RECT r = {0, 0, desc->width, desc->height};
    AdjustWindowRect(&r, style, FALSE);
    const int win_w = (int)(r.right - r.left);
    const int win_h = (int)(r.bottom - r.top);

    wchar_t *title_w = ne_utf8_to_wide(desc->title ? desc->title : "NanoEngine");

    const HWND hwnd = CreateWindowExW(0, L"NanoEngineWindow", title_w ? title_w : L"NanoEngine", style, (int)desc->x, (int)desc->y,
                                      win_w, win_h, NULL, NULL, app->hinstance, window);

    if (title_w) {
        free(title_w);
    }

    if (!hwnd) {
        const DWORD err = GetLastError();
        NE_LOG_ERROR("CreateWindowExW failed (err=%lu)", (unsigned long)err);
        free(window);
        return NULL;
    }

    window->hwnd = hwnd;

    app->window_count++;
    return window;
}

void ne_window_destroy(NEWindow *window) {
    if (!window) {
        return;
    }

    window->destroying = true;

    /* Do not emit user callbacks during teardown. */
    memset(&window->callbacks, 0, sizeof(window->callbacks));
    window->user_data = NULL;

    if (window->hwnd) {
        DestroyWindow(window->hwnd);
        window->hwnd = NULL;
    }

    free(window);
}

void ne_window_show(NEWindow *window) {
    if (!window || !window->hwnd) {
        return;
    }
    ShowWindow(window->hwnd, SW_SHOW);
    UpdateWindow(window->hwnd);
}

void ne_window_hide(NEWindow *window) {
    if (!window || !window->hwnd) {
        return;
    }
    ShowWindow(window->hwnd, SW_HIDE);
}

void ne_window_request_close(NEWindow *window) {
    if (!window || !window->hwnd) {
        return;
    }
    PostMessageW(window->hwnd, WM_CLOSE, 0, 0);
}

void ne_window_set_title(NEWindow *window, const char *title) {
    if (!window || !window->hwnd || !title) {
        return;
    }
    wchar_t *w = ne_utf8_to_wide(title);
    if (!w) {
        return;
    }
    SetWindowTextW(window->hwnd, w);
    free(w);
}

void ne_window_set_position(NEWindow *window, int32_t x, int32_t y) {
    if (!window || !window->hwnd) {
        return;
    }
    SetWindowPos(window->hwnd, NULL, (int)x, (int)y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ne_window_set_size(NEWindow *window, int32_t width, int32_t height) {
    if (!window || !window->hwnd || width <= 0 || height <= 0) {
        return;
    }

    DWORD style = (DWORD)GetWindowLongPtrW(window->hwnd, GWL_STYLE);
    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, style, FALSE);
    const int win_w = (int)(r.right - r.left);
    const int win_h = (int)(r.bottom - r.top);

    SetWindowPos(window->hwnd, NULL, 0, 0, win_w, win_h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void ne_window_set_callbacks(NEWindow *window, const NEWindowCallbacks *callbacks, void *user_data) {
    if (!window) {
        return;
    }

    if (callbacks) {
        window->callbacks = *callbacks;
    } else {
        memset(&window->callbacks, 0, sizeof(window->callbacks));
    }
    window->user_data = user_data;
}

bool ne_window_get_content_size(const NEWindow *window, int32_t *out_width, int32_t *out_height) {
    if (!window || !window->open || !window->hwnd) {
        return false;
    }

    RECT r;
    if (!GetClientRect(window->hwnd, &r)) {
        return false;
    }

    if (out_width) {
        *out_width = (int32_t)(r.right - r.left);
    }
    if (out_height) {
        *out_height = (int32_t)(r.bottom - r.top);
    }

    return true;
}

bool ne_window_get_framebuffer_size(const NEWindow *window, int32_t *out_width, int32_t *out_height) {
    /* DPI handling is deferred; treat framebuffer==content for now. */
    return ne_window_get_content_size(window, out_width, out_height);
}

bool ne_window_get_content_scale(const NEWindow *window, float *out_scale) {
    if (!window || !window->open || !out_scale) {
        return false;
    }
    *out_scale = 1.0f;
    return true;
}

void *ne_window_get_native_handle(NEWindow *window, NENativeHandleType type) {
    if (!window || !window->open) {
        return NULL;
    }

    switch (type) {
    case NE_NATIVE_HANDLE_WIN32_HWND:
        return (void *)window->hwnd;

    case NE_NATIVE_HANDLE_COCOA_NS_WINDOW:
    case NE_NATIVE_HANDLE_COCOA_NS_VIEW:
    case NE_NATIVE_HANDLE_X11_WINDOW:
    case NE_NATIVE_HANDLE_WAYLAND_SURFACE:
    default:
        NE_ASSERT(false && "Requested native handle type is not supported on this platform/build");
        return NULL;
    }
}

bool ne_window_is_mouse_button_down(const NEWindow *window, NEMouseButton button) {
    if (!window) {
        return false;
    }
    if ((uint32_t)button >= 32u) {
        return false;
    }
    return (window->mouse_buttons_down_mask & (1u << (uint32_t)button)) != 0;
}

bool ne_window_is_open(const NEWindow *window) {
    return window && window->open;
}
