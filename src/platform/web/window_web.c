/*
 * Web platform backend (Emscripten / browser).
 *
 * Implements ne_app_* and ne_window_* over an HTML <canvas> and the browser's
 * requestAnimationFrame loop. This is the web analogue of window_macos.m: the
 * frame loop is PUSH (emscripten_set_main_loop drives ne_render_frame via the
 * per-window render dispatch), matching the macOS CADisplayLink model and the
 * project's established frame-driving model.
 *
 * Cannot be compiled in this environment (no Emscripten toolchain). Written
 * correct-by-construction against emscripten/html5.h; verify against the pinned
 * emsdk. TODO(web) markers note places a fuller backend would extend.
 */

#if !defined(__EMSCRIPTEN__)
#error "Web platform backend targets Emscripten (build with PLATFORM=web)"
#endif

#include "ne_app.h"
#include "ne_log.h"
#include "ne_window.h"

#include <emscripten.h>
#include <emscripten/html5.h>

#include <stdlib.h>
#include <string.h>

/*
 * The canvas the engine renders to. Emscripten's default canvas element id is
 * "canvas"; the CSS/event selector form is "#canvas". The WebGPU backend uses
 * the same default selector for surface creation.
 */
#define NE_WEB_CANVAS_TARGET   "#canvas"

struct NEApp {
    bool initialized;
    bool running;
    uint32_t window_count;

    /* Intrusive list of live windows (mirrors the native backends). */
    NEWindow *windows;
};

static struct {
    NEApp *app;
    uint32_t refcount;
} g_app_state = {0};

struct NEWindow {
    NEApp *app;

    NEWindowCallbacks callbacks;
    void *user_data;

    bool open;

    int32_t width;  /* CSS pixels */
    int32_t height; /* CSS pixels */

    uint32_t mouse_buttons_down_mask;

    void (*present_frame)(void);
    void (*render_frame)(void *user);
    void *render_frame_user;

    NEWindow *next; /* link in NEApp::windows registry */
};

/* ── App lifecycle ──────────────────────────────────────────────────────── */

NEApp *ne_app_create(void) {
    if (g_app_state.app) {
        g_app_state.refcount++;
        return g_app_state.app;
    }

    NEApp *app = (NEApp *)calloc(1, sizeof(NEApp));
    if (!app) {
        return NULL;
    }

    app->initialized = true;
    app->running = true;
    app->window_count = 0;

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

    free(app);
    g_app_state.app = NULL;
}

bool ne_app_poll_events(NEApp *app) {
    /*
     * In the browser, the event loop is owned by the page; input is delivered
     * via the html5.h event callbacks registered in ne_window_create, not by a
     * manual pump. This remains callable (manual-loop escape hatch) but does no
     * work beyond reporting the running state.
     */
    if (!app || !app->initialized || !app->running) {
        return false;
    }
    if (app->window_count == 0) {
        app->running = false;
    }
    return app->running;
}

/*
 * rAF tick: the browser calls this once per display refresh (the push frame
 * driver). It invokes every open window's render dispatch — the web analogue of
 * the macOS CADisplayLink callback.
 */
static void ne_web_frame_tick(void *user) {
    NEApp *app = (NEApp *)user;
    if (!app) {
        return;
    }
    for (NEWindow *w = app->windows; w; w = w->next) {
        if (w->open && w->render_frame) {
            w->render_frame(w->render_frame_user);
        }
    }
}

void ne_app_run(NEApp *app) {
    if (!app) {
        return;
    }
    /*
     * Hand the loop to the browser. fps=0 means "use requestAnimationFrame";
     * simulate_infinite_loop=1 means this call does not return — main() unwinds
     * via a longjmp-like mechanism while the rAF loop keeps running. Combined
     * with -sEXIT_RUNTIME=0, heap objects (renderer/app) stay alive.
     *
     * IMPORTANT: because main() unwinds, any state the render callback touches
     * must NOT live on main()'s stack (see the static frame context in main.c).
     */
    emscripten_set_main_loop_arg(ne_web_frame_tick, app, 0, 1);
}

void ne_app_request_quit(NEApp *app) {
    if (!app) {
        return;
    }
    app->running = false;
    /* TODO(web): emscripten_cancel_main_loop() to actually stop the rAF loop. */
}

bool ne_app_is_running(const NEApp *app) {
    return app && app->running;
}

uint32_t ne_app_get_window_count(const NEApp *app) {
    return app ? app->window_count : 0;
}

/* ── Input event handlers ───────────────────────────────────────────────── */

static NEMouseButton ne_web_map_mouse_button(unsigned short button) {
    switch (button) {
    case 0:  return NE_MOUSE_BUTTON_LEFT;
    case 1:  return NE_MOUSE_BUTTON_MIDDLE; /* DOM: 1 = middle */
    case 2:  return NE_MOUSE_BUTTON_RIGHT;  /* DOM: 2 = right  */
    default: return NE_MOUSE_BUTTON_OTHER;
    }
}

static NEModifiers ne_web_map_modifiers(const EmscriptenMouseEvent *e) {
    NEModifiers mods = 0;
    if (e->shiftKey) mods |= NE_MOD_SHIFT;
    if (e->ctrlKey)  mods |= NE_MOD_CONTROL;
    if (e->altKey)   mods |= NE_MOD_ALT;
    if (e->metaKey)  mods |= NE_MOD_SUPER;
    return mods;
}

/* Map a DOM KeyboardEvent.code string to an NEKey. Covers the same subset as
 * the native backends; TODO(web) extend as needed. */
static NEKey ne_web_map_key(const char *code) {
    if (!code) return NE_KEY_UNKNOWN;
    if (strncmp(code, "Key", 3) == 0 && code[3] >= 'A' && code[3] <= 'Z' && code[4] == '\0') {
        return (NEKey)(NE_KEY_A + (code[3] - 'A'));
    }
    if (strncmp(code, "Digit", 5) == 0 && code[5] >= '0' && code[5] <= '9' && code[6] == '\0') {
        return (NEKey)(NE_KEY_0 + (code[5] - '0'));
    }
    if (strcmp(code, "Escape") == 0)     return NE_KEY_ESCAPE;
    if (strcmp(code, "Enter") == 0)      return NE_KEY_ENTER;
    if (strcmp(code, "Tab") == 0)        return NE_KEY_TAB;
    if (strcmp(code, "Backspace") == 0)  return NE_KEY_BACKSPACE;
    if (strcmp(code, "Space") == 0)      return NE_KEY_SPACE;
    if (strcmp(code, "ArrowLeft") == 0)  return NE_KEY_LEFT;
    if (strcmp(code, "ArrowRight") == 0) return NE_KEY_RIGHT;
    if (strcmp(code, "ArrowUp") == 0)    return NE_KEY_UP;
    if (strcmp(code, "ArrowDown") == 0)  return NE_KEY_DOWN;
    return NE_KEY_UNKNOWN;
}

static EM_BOOL ne_web_on_key(int event_type, const EmscriptenKeyboardEvent *e, void *user) {
    NEWindow *window = (NEWindow *)user;
    if (!window || !window->open) {
        return EM_FALSE;
    }

    NEKeyEvent ke;
    memset(&ke, 0, sizeof(ke));
    ke.key = ne_web_map_key(e->code);
    ke.repeat = e->repeat ? true : false;
    if (e->shiftKey) ke.modifiers |= NE_MOD_SHIFT;
    if (e->ctrlKey)  ke.modifiers |= NE_MOD_CONTROL;
    if (e->altKey)   ke.modifiers |= NE_MOD_ALT;
    if (e->metaKey)  ke.modifiers |= NE_MOD_SUPER;

    if (event_type == EMSCRIPTEN_EVENT_KEYDOWN) {
        if (window->callbacks.on_key_down) {
            window->callbacks.on_key_down(window, ke, window->user_data);
        }
    } else if (event_type == EMSCRIPTEN_EVENT_KEYUP) {
        if (window->callbacks.on_key_up) {
            window->callbacks.on_key_up(window, ke, window->user_data);
        }
    }
    return EM_TRUE;
}

static EM_BOOL ne_web_on_mouse(int event_type, const EmscriptenMouseEvent *e, void *user) {
    NEWindow *window = (NEWindow *)user;
    if (!window || !window->open) {
        return EM_FALSE;
    }

    const float x = (float)e->targetX;
    const float y = (float)e->targetY;

    if (event_type == EMSCRIPTEN_EVENT_MOUSEMOVE) {
        if (window->callbacks.on_mouse_move) {
            NEMouseMoveEvent me = {
                .x = x, .y = y,
                .delta_x = (float)e->movementX,
                .delta_y = (float)e->movementY,
            };
            window->callbacks.on_mouse_move(window, me, window->user_data);
        }
        return EM_TRUE;
    }

    const NEMouseButton button = ne_web_map_mouse_button(e->button);
    if ((uint32_t)button < 32u) {
        const uint32_t bit = 1u << (uint32_t)button;
        if (event_type == EMSCRIPTEN_EVENT_MOUSEDOWN) {
            window->mouse_buttons_down_mask |= bit;
        } else {
            window->mouse_buttons_down_mask &= ~bit;
        }
    }

    NEMouseButtonEvent be = {
        .button = button, .x = x, .y = y, .modifiers = ne_web_map_modifiers(e),
    };
    if (event_type == EMSCRIPTEN_EVENT_MOUSEDOWN && window->callbacks.on_mouse_down) {
        window->callbacks.on_mouse_down(window, be, window->user_data);
    } else if (event_type == EMSCRIPTEN_EVENT_MOUSEUP && window->callbacks.on_mouse_up) {
        window->callbacks.on_mouse_up(window, be, window->user_data);
    }
    return EM_TRUE;
}

static EM_BOOL ne_web_on_wheel(int event_type, const EmscriptenWheelEvent *e, void *user) {
    (void)event_type;
    NEWindow *window = (NEWindow *)user;
    if (!window || !window->open || !window->callbacks.on_mouse_scroll) {
        return EM_FALSE;
    }
    /* DOM deltaY is positive when scrolling down; negate to match the engine's
     * "positive = up / away from user" convention. ~100px per notch → ±1. */
    NEMouseScrollEvent se = {
        .delta_x = -(float)(e->deltaX / 100.0),
        .delta_y = -(float)(e->deltaY / 100.0),
    };
    window->callbacks.on_mouse_scroll(window, se, window->user_data);
    return EM_TRUE;
}

static EM_BOOL ne_web_on_resize(int event_type, const EmscriptenUiEvent *e, void *user) {
    (void)event_type;
    (void)e;
    NEWindow *window = (NEWindow *)user;
    if (!window || !window->open) {
        return EM_FALSE;
    }

    double css_w = 0.0, css_h = 0.0;
    if (emscripten_get_element_css_size(NE_WEB_CANVAS_TARGET, &css_w, &css_h) == EMSCRIPTEN_RESULT_SUCCESS) {
        window->width = (int32_t)css_w;
        window->height = (int32_t)css_h;
        /* Keep the canvas drawing-buffer in physical pixels (DPR-scaled). */
        const double dpr = emscripten_get_device_pixel_ratio();
        emscripten_set_canvas_element_size(NE_WEB_CANVAS_TARGET,
                                           (int)(css_w * dpr), (int)(css_h * dpr));
        if (window->callbacks.on_resize) {
            window->callbacks.on_resize(window, window->width, window->height, window->user_data);
        }
    }
    return EM_TRUE;
}

/* ── Window lifecycle ───────────────────────────────────────────────────── */

NEWindow *ne_window_create(NEApp *app, const NEWindowDesc *desc) {
    if (!app || !app->initialized || !app->running || !desc || desc->width <= 0 || desc->height <= 0) {
        return NULL;
    }

    NEWindow *window = (NEWindow *)calloc(1, sizeof(NEWindow));
    if (!window) {
        return NULL;
    }

    window->app = app;
    window->width = desc->width;
    window->height = desc->height;
    window->open = true;

    /* Size the canvas drawing buffer to the requested size × device pixel ratio. */
    const double dpr = emscripten_get_device_pixel_ratio();
    emscripten_set_canvas_element_size(NE_WEB_CANVAS_TARGET,
                                       (int)(desc->width * dpr), (int)(desc->height * dpr));

    /* Register input + resize callbacks on the canvas / window.
     * useCapture = EM_TRUE; the window pointer is the user data. */
    emscripten_set_keydown_callback(NE_WEB_CANVAS_TARGET, window, EM_TRUE, ne_web_on_key);
    emscripten_set_keyup_callback(NE_WEB_CANVAS_TARGET, window, EM_TRUE, ne_web_on_key);
    emscripten_set_mousedown_callback(NE_WEB_CANVAS_TARGET, window, EM_TRUE, ne_web_on_mouse);
    emscripten_set_mouseup_callback(NE_WEB_CANVAS_TARGET, window, EM_TRUE, ne_web_on_mouse);
    emscripten_set_mousemove_callback(NE_WEB_CANVAS_TARGET, window, EM_TRUE, ne_web_on_mouse);
    emscripten_set_wheel_callback(NE_WEB_CANVAS_TARGET, window, EM_TRUE, ne_web_on_wheel);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, window, EM_TRUE, ne_web_on_resize);

    /* Register in the app's window list (head-insert). */
    window->next = app->windows;
    app->windows = window;
    app->window_count++;

    return window;
}

void ne_window_destroy(NEWindow *window) {
    if (!window) {
        return;
    }

    /* Unlink from the app's window registry. */
    if (window->app) {
        NEWindow **pp = &window->app->windows;
        while (*pp) {
            if (*pp == window) {
                *pp = window->next;
                break;
            }
            pp = &(*pp)->next;
        }
        window->next = NULL;
    }

    /* Detach input callbacks (pass NULL handler). */
    emscripten_set_keydown_callback(NE_WEB_CANVAS_TARGET, NULL, EM_TRUE, NULL);
    emscripten_set_keyup_callback(NE_WEB_CANVAS_TARGET, NULL, EM_TRUE, NULL);
    emscripten_set_mousedown_callback(NE_WEB_CANVAS_TARGET, NULL, EM_TRUE, NULL);
    emscripten_set_mouseup_callback(NE_WEB_CANVAS_TARGET, NULL, EM_TRUE, NULL);
    emscripten_set_mousemove_callback(NE_WEB_CANVAS_TARGET, NULL, EM_TRUE, NULL);
    emscripten_set_wheel_callback(NE_WEB_CANVAS_TARGET, NULL, EM_TRUE, NULL);
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, NULL);

    if (window->open) {
        window->open = false;
        if (window->app && window->app->window_count > 0) {
            window->app->window_count--;
        }
    }

    free(window);
}

void ne_window_show(NEWindow *window) {
    (void)window; /* The canvas is always visible in the page. */
}

void ne_window_hide(NEWindow *window) {
    (void)window; /* TODO(web): toggle canvas CSS display if needed. */
}

void ne_window_request_close(NEWindow *window) {
    if (!window || !window->open) {
        return;
    }
    window->open = false;
    if (window->app && window->app->window_count > 0) {
        window->app->window_count--;
    }
    if (window->callbacks.on_close) {
        window->callbacks.on_close(window, window->user_data);
    }
}

void ne_window_set_title(NEWindow *window, const char *title) {
    (void)window;
    if (title) {
        emscripten_set_window_title(title); /* sets document.title */
    }
}

void ne_window_set_position(NEWindow *window, int32_t x, int32_t y) {
    (void)window;
    (void)x;
    (void)y;
    /* Not meaningful for a canvas in a page. */
}

void ne_window_set_size(NEWindow *window, int32_t width, int32_t height) {
    if (!window || width <= 0 || height <= 0) {
        return;
    }
    window->width = width;
    window->height = height;
    const double dpr = emscripten_get_device_pixel_ratio();
    emscripten_set_canvas_element_size(NE_WEB_CANVAS_TARGET, (int)(width * dpr), (int)(height * dpr));
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
    if (!window || !window->open) {
        return false;
    }
    if (out_width) *out_width = window->width;
    if (out_height) *out_height = window->height;
    return true;
}

bool ne_window_get_framebuffer_size(const NEWindow *window, int32_t *out_width, int32_t *out_height) {
    if (!window || !window->open) {
        return false;
    }
    /* The framebuffer is the canvas drawing-buffer size in physical pixels. */
    int w = 0, h = 0;
    if (emscripten_get_canvas_element_size(NE_WEB_CANVAS_TARGET, &w, &h) != EMSCRIPTEN_RESULT_SUCCESS) {
        return false;
    }
    if (out_width) *out_width = (int32_t)w;
    if (out_height) *out_height = (int32_t)h;
    return true;
}

bool ne_window_get_content_scale(const NEWindow *window, float *out_scale) {
    if (!window || !window->open || !out_scale) {
        return false;
    }
    *out_scale = (float)emscripten_get_device_pixel_ratio();
    return true;
}

void *ne_window_get_native_handle(NEWindow *window, NENativeHandleType type) {
    (void)window;
    (void)type;
    /*
     * The WebGPU backend creates its surface from the canvas selector directly
     * (NE_WEB_CANVAS_TARGET); there is no native-handle type for a web canvas
     * yet. TODO(web): add NE_NATIVE_HANDLE_WEB_CANVAS_SELECTOR and return the
     * selector string here.
     */
    return NULL;
}

bool ne_window_is_mouse_button_down(const NEWindow *window, NEMouseButton button) {
    if (!window || (uint32_t)button >= 32u) {
        return false;
    }
    return (window->mouse_buttons_down_mask & (1u << (uint32_t)button)) != 0;
}

bool ne_window_is_open(const NEWindow *window) {
    return window && window->open;
}

void ne_window_invalidate(const NEWindow *window) {
    (void)window; /* The rAF loop redraws every frame; nothing to invalidate. */
}

void ne_set_window_present_dispatch(NEWindow *window, void (*presentFrame)(void)) {
    if (!window) {
        return;
    }
    window->present_frame = presentFrame;
}

void ne_set_window_render_dispatch(NEWindow *window, void (*render)(void *user), void *user) {
    if (!window) {
        return;
    }
    window->render_frame = render;
    window->render_frame_user = user;
}
