#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Carbon/Carbon.h> /*for kVK_* key code constants */

#include "ne_app.h"
#include "ne_log.h"
#include "ne_window.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct NEApp {
    bool initialized;
    bool running;
    uint32_t window_count;
};

static struct {
    NEApp *app;
    uint32_t refcount;
} g_app_state = {0};

struct NEWindow {
    NEApp *app;
    void *ns_window;
    void *content_view;
    void *delegate;

    NEWindowCallbacks callbacks;
    void *user_data;
    bool open;
    bool destroying;

    uint32_t mouse_buttons_down_mask;

    float last_mouse_x;
    float last_mouse_y;
    bool has_last_mouse;
};

static NEMouseButton ne_map_mouse_button(NSInteger button) {
    switch (button) {
    case 0:
        return NE_MOUSE_BUTTON_LEFT;
    case 1:
        return NE_MOUSE_BUTTON_RIGHT;
    case 2:
        return NE_MOUSE_BUTTON_MIDDLE;
    default:
        return NE_MOUSE_BUTTON_OTHER;
    }
}

static NEModifiers ne_map_modifiers(NSEventModifierFlags flags) {
    NEModifiers mods = 0;
    if (flags & NSEventModifierFlagShift) {
        mods |= NE_MOD_SHIFT;
    }
    if (flags & NSEventModifierFlagControl) {
        mods |= NE_MOD_CONTROL;
    }
    if (flags & NSEventModifierFlagOption) {
        mods |= NE_MOD_ALT;
    }
    if (flags & NSEventModifierFlagCommand) {
        mods |= NE_MOD_SUPER;
    }
    if (flags & NSEventModifierFlagCapsLock) {
        mods |= NE_MOD_CAPS_LOCK;
    }
    return mods;
}

static NEKey ne_map_key(uint16_t key_code) {
    /*
     * macOS `[NSEvent keyCode]` returns a "virtual key code" (kVK_*) that
     * mostly corresponds to physical key positions on an ANSI keyboard, not a
     * localized character.
     *
     * References:
     * - <HIToolbox/Events.h> (included via <Carbon/Carbon.h>)
     */
    switch (key_code) {
    /* Letters */
    case kVK_ANSI_A:
        return NE_KEY_A;
    case kVK_ANSI_B:
        return NE_KEY_B;
    case kVK_ANSI_C:
        return NE_KEY_C;
    case kVK_ANSI_D:
        return NE_KEY_D;
    case kVK_ANSI_E:
        return NE_KEY_E;
    case kVK_ANSI_F:
        return NE_KEY_F;
    case kVK_ANSI_G:
        return NE_KEY_G;
    case kVK_ANSI_H:
        return NE_KEY_H;
    case kVK_ANSI_I:
        return NE_KEY_I;
    case kVK_ANSI_J:
        return NE_KEY_J;
    case kVK_ANSI_K:
        return NE_KEY_K;
    case kVK_ANSI_L:
        return NE_KEY_L;
    case kVK_ANSI_M:
        return NE_KEY_M;
    case kVK_ANSI_N:
        return NE_KEY_N;
    case kVK_ANSI_O:
        return NE_KEY_O;
    case kVK_ANSI_P:
        return NE_KEY_P;
    case kVK_ANSI_Q:
        return NE_KEY_Q;
    case kVK_ANSI_R:
        return NE_KEY_R;
    case kVK_ANSI_S:
        return NE_KEY_S;
    case kVK_ANSI_T:
        return NE_KEY_T;
    case kVK_ANSI_U:
        return NE_KEY_U;
    case kVK_ANSI_V:
        return NE_KEY_V;
    case kVK_ANSI_W:
        return NE_KEY_W;
    case kVK_ANSI_X:
        return NE_KEY_X;
    case kVK_ANSI_Y:
        return NE_KEY_Y;
    case kVK_ANSI_Z:
        return NE_KEY_Z;

    /* Digits */
    case kVK_ANSI_0:
        return NE_KEY_0;
    case kVK_ANSI_1:
        return NE_KEY_1;
    case kVK_ANSI_2:
        return NE_KEY_2;
    case kVK_ANSI_3:
        return NE_KEY_3;
    case kVK_ANSI_4:
        return NE_KEY_4;
    case kVK_ANSI_5:
        return NE_KEY_5;
    case kVK_ANSI_6:
        return NE_KEY_6;
    case kVK_ANSI_7:
        return NE_KEY_7;
    case kVK_ANSI_8:
        return NE_KEY_8;
    case kVK_ANSI_9:
        return NE_KEY_9;

    /* Controls */
    case kVK_Escape:
        return NE_KEY_ESCAPE;
    case kVK_Return:
        return NE_KEY_ENTER;
    case kVK_Tab:
        return NE_KEY_TAB;
    case kVK_Delete:
        return NE_KEY_BACKSPACE;
    case kVK_Space:
        return NE_KEY_SPACE;

    /* Arrows */
    case kVK_LeftArrow:
        return NE_KEY_LEFT;
    case kVK_RightArrow:
        return NE_KEY_RIGHT;
    case kVK_UpArrow:
        return NE_KEY_UP;
    case kVK_DownArrow:
        return NE_KEY_DOWN;

    default:
        return NE_KEY_UNKNOWN;
    }
}

static void ne_emit_text_input(NEWindow *window, NSString *text) {
    if (!window || !window->callbacks.on_text_input || !text) {
        return;
    }

    /*
     * `NSString` is UTF-16; convert to Unicode scalar values and emit a callback
     * per codepoint.
     */
    const NSUInteger len = [text length];
    for (NSUInteger i = 0; i < len; i++) {
        const unichar c = [text characterAtIndex:i];
        uint32_t codepoint = (uint32_t)c;

        if (CFStringIsSurrogateHighCharacter(c) && (i + 1) < len) {
            const unichar low = [text characterAtIndex:(i + 1)];
            if (CFStringIsSurrogateLowCharacter(low)) {
                codepoint = (uint32_t)CFStringGetLongCharacterForSurrogatePair(c, low);
                i++; /* consumed the low surrogate */
            }
        }

        window->callbacks.on_text_input(window, codepoint, window->user_data);
    }
}

static void ne_emit_mouse_move(NEWindow *window, NSEvent *event) {
    if (!window || !window->callbacks.on_mouse_move) {
        return;
    }
    NSView *view = (__bridge NSView *)window->content_view;
    NSPoint p = [view convertPoint:[event locationInWindow] fromView:nil];
    NEMouseMoveEvent e = {
        .x = (float)p.x,
        .y = (float)p.y,
        .delta_x = window->has_last_mouse ? (float)(p.x - window->last_mouse_x) : 0.0f,
        .delta_y = window->has_last_mouse ? (float)(p.y - window->last_mouse_y) : 0.0f,
    };
    window->last_mouse_x = (float)p.x;
    window->last_mouse_y = (float)p.y;
    window->has_last_mouse = true;
    window->callbacks.on_mouse_move(window, e, window->user_data);
}

static void ne_emit_mouse_button(NEWindow *window, NSEvent *event, bool is_down, NEOnMouseButtonFn callback) {
    if (!window || !event) {
        return;
    }

    const NEMouseButton button = ne_map_mouse_button([event buttonNumber]);
    if ((uint32_t)button < 32u) {
        const uint32_t bit = 1u << (uint32_t)button;
        if (is_down) {
            window->mouse_buttons_down_mask |= bit;
        } else {
            window->mouse_buttons_down_mask &= ~bit;
        }
    }

    if (!callback) {
        return;
    }

    NSView *view = (__bridge NSView *)window->content_view;
    NSPoint p = [view convertPoint:[event locationInWindow] fromView:nil];
    NEMouseButtonEvent e = {
        .button = button,
        .x = (float)p.x,
        .y = (float)p.y,
        .modifiers = ne_map_modifiers([event modifierFlags]),
    };
    callback(window, e, window->user_data);
}

/**
 * Custom NSWindow subclass that allows borderless windows to become key.
 *
 * By default, an NSWindow with NSWindowStyleMaskBorderless returns NO from
 * -canBecomeKeyWindow, which prevents it from receiving keyboard events.
 */
@interface NEMacWindow : NSWindow
@end

@implementation NEMacWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
@end

@interface NEMacWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) NEWindow *owner;
@end

@interface NEMacView : NSView
@property(nonatomic, assign) NEWindow *owner;
@end

@implementation NEMacWindowDelegate

- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    if (!self.owner || !self.owner->open) {
        return;
    }

    self.owner->open = false;
    if (self.owner->app && self.owner->app->window_count > 0) {
        self.owner->app->window_count--;
    }

    /* Avoid firing user callbacks during programmatic teardown. */
    if (!self.owner->destroying && self.owner->callbacks.on_close) {
        self.owner->callbacks.on_close(self.owner, self.owner->user_data);
    }
}

- (void)windowDidResize:(NSNotification *)notification {
    if (!self.owner || !self.owner->callbacks.on_resize) {
        return;
    }
    NSWindow *w = [notification object];
    NSRect content = [w contentRectForFrameRect:[w frame]];
    self.owner->callbacks.on_resize(self.owner, (int32_t)content.size.width, (int32_t)content.size.height,
                                    self.owner->user_data);
}

- (void)windowDidMove:(NSNotification *)notification {
    if (!self.owner || !self.owner->callbacks.on_move) {
        return;
    }
    NSWindow *w = [notification object];
    NSRect frame = [w frame];
    self.owner->callbacks.on_move(self.owner, (int32_t)frame.origin.x, (int32_t)frame.origin.y, self.owner->user_data);
}

@end

@implementation NEMacView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    if (!self.owner) {
        return;
    }

    const uint16_t native_key_code = (uint16_t)[event keyCode];
    const NSEventModifierFlags ns_mods = [event modifierFlags];

    NEKeyEvent e = {
        .key = ne_map_key(native_key_code),
        .native_key_code = native_key_code,
        .modifiers = ne_map_modifiers(ns_mods),
        .repeat = [event isARepeat] ? true : false,
    };

    /* Raw key callback (physical key). */
    if (self.owner->callbacks.on_key_down) {
        self.owner->callbacks.on_key_down(self.owner, e, self.owner->user_data);
    }

    /* Text input callback (layout / IME output). */
    if (self.owner->callbacks.on_text_input) {
        const bool has_command_or_control = (ns_mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) != 0;

        /* Skip text emission for common non-text keys. */
        const bool is_non_text_key = (e.key == NE_KEY_ESCAPE) || (e.key == NE_KEY_BACKSPACE) || (e.key == NE_KEY_LEFT) ||
                                    (e.key == NE_KEY_RIGHT) || (e.key == NE_KEY_UP) || (e.key == NE_KEY_DOWN);

        if (!has_command_or_control && !is_non_text_key) {
            NSString *chars = [event characters];
            if (chars && [chars length] > 0) {
                ne_emit_text_input(self.owner, chars);
            }
        }
    }
}

- (void)keyUp:(NSEvent *)event {
    if (!self.owner) {
        return;
    }

    if (!self.owner->callbacks.on_key_up) {
        return;
    }

    const uint16_t native_key_code = (uint16_t)[event keyCode];
    NEKeyEvent e = {
        .key = ne_map_key(native_key_code),
        .native_key_code = native_key_code,
        .modifiers = ne_map_modifiers([event modifierFlags]),
        .repeat = false,
    };
    self.owner->callbacks.on_key_up(self.owner, e, self.owner->user_data);
}

- (void)mouseMoved:(NSEvent *)event {
    ne_emit_mouse_move(self.owner, event);
}

- (void)mouseDragged:(NSEvent *)event {
    ne_emit_mouse_move(self.owner, event);
}

- (void)rightMouseDragged:(NSEvent *)event {
    ne_emit_mouse_move(self.owner, event);
}

- (void)otherMouseDragged:(NSEvent *)event {
    ne_emit_mouse_move(self.owner, event);
}

- (void)mouseDown:(NSEvent *)event {
    ne_emit_mouse_button(self.owner, event, true, self.owner ? self.owner->callbacks.on_mouse_down : NULL);
}

- (void)mouseUp:(NSEvent *)event {
    ne_emit_mouse_button(self.owner, event, false, self.owner ? self.owner->callbacks.on_mouse_up : NULL);
}

- (void)rightMouseDown:(NSEvent *)event {
    ne_emit_mouse_button(self.owner, event, true, self.owner ? self.owner->callbacks.on_mouse_down : NULL);
}

- (void)rightMouseUp:(NSEvent *)event {
    ne_emit_mouse_button(self.owner, event, false, self.owner ? self.owner->callbacks.on_mouse_up : NULL);
}

- (void)otherMouseDown:(NSEvent *)event {
    ne_emit_mouse_button(self.owner, event, true, self.owner ? self.owner->callbacks.on_mouse_down : NULL);
}

- (void)otherMouseUp:(NSEvent *)event {
    ne_emit_mouse_button(self.owner, event, false, self.owner ? self.owner->callbacks.on_mouse_up : NULL);
}

- (void)scrollWheel:(NSEvent *)event {
    if (!self.owner || !self.owner->callbacks.on_mouse_scroll) {
        return;
    }
    NEMouseScrollEvent e = {
        .delta_x = (float)[event scrollingDeltaX],
        .delta_y = (float)[event scrollingDeltaY],
    };
    self.owner->callbacks.on_mouse_scroll(self.owner, e, self.owner->user_data);
}

@end

NEApp *ne_app_create(void) {
    if (g_app_state.app) {
        g_app_state.refcount++;
        return g_app_state.app;
    }

    NEApp *app = (NEApp *)calloc(1, sizeof(NEApp));
    if (!app) {
        return NULL;
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSMenu *menubar = [[NSMenu alloc] init];
    NSMenuItem *app_menu_item = [[NSMenuItem alloc] init];
    [menubar addItem:app_menu_item];
    [NSApp setMainMenu:menubar];

    NSMenu *app_menu = [[NSMenu alloc] init];
    NSString *process_name = [[NSProcessInfo processInfo] processName];
    NSString *quit_title = [@"Quit " stringByAppendingString:process_name];
    NSMenuItem *quit_item = [[NSMenuItem alloc] initWithTitle:quit_title action:@selector(terminate:) keyEquivalent:@"q"];
    [app_menu addItem:quit_item];
    [app_menu_item setSubmenu:app_menu];

    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];

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
    if (!app || !app->initialized || !app->running) {
        return false;
    }

    @autoreleasepool {
        while (true) {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.0]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event) {
                break;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }

    if (app->window_count == 0) {
        app->running = false;
    }
    return app->running;
}

void ne_app_run(NEApp *app) {
    const struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 1000000};
    while (ne_app_poll_events(app)) {
        nanosleep(&sleep_time, NULL);
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

    NEWindow *window = (NEWindow *)calloc(1, sizeof(NEWindow));
    if (!window) {
        return NULL;
    }

    window->app = app;

    NSUInteger style;
    if (desc->undecorated) {
        style = NSWindowStyleMaskBorderless;
    } else {
        style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
        if (desc->resizable) {
            style |= NSWindowStyleMaskResizable;
        }
    }

    NSRect frame = NSMakeRect((CGFloat)desc->x, (CGFloat)desc->y, (CGFloat)desc->width, (CGFloat)desc->height);
    NSWindow *ns_window = [[NEMacWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
    if (!ns_window) {
        free(window);
        return NULL;
    }

    NSString *title = desc->title ? [NSString stringWithUTF8String:desc->title] : @"NanoEngine";
    [ns_window setTitle:title];

    NEMacView *view = [[NEMacView alloc] initWithFrame:[[ns_window contentView] bounds]];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [view setWantsLayer:YES];

    NEMacWindowDelegate *delegate = [[NEMacWindowDelegate alloc] init];
    delegate.owner = window;
    view.owner = window;

    [ns_window setDelegate:delegate];
    [ns_window setContentView:view];
    [ns_window setAcceptsMouseMovedEvents:YES];
    [ns_window makeFirstResponder:view];

    window->ns_window = (__bridge_retained void *)ns_window;
    window->content_view = (__bridge_retained void *)view;
    window->delegate = (__bridge_retained void *)delegate;
    window->open = true;
    window->destroying = false;

    app->window_count++;

    if (desc->show_on_create) {
        [ns_window makeKeyAndOrderFront:nil];
    }

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

    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    NEMacWindowDelegate *delegate = window->delegate ? (__bridge NEMacWindowDelegate *)window->delegate : nil;
    NEMacView *view = window->content_view ? (__bridge NEMacView *)window->content_view : nil;

    if (delegate) {
        delegate.owner = NULL;
    }
    if (view) {
        view.owner = NULL;
    }

    if (ns_window) {
        [ns_window setDelegate:nil];
        [ns_window setContentView:nil];
    }

    if (window->open) {
        window->open = false;
        if (window->app && window->app->window_count > 0) {
            window->app->window_count--;
        }
    }

    if (ns_window) {
        [ns_window close];
    }

    if (window->delegate) {
        (void)CFBridgingRelease(window->delegate);
        window->delegate = NULL;
    }
    if (window->content_view) {
        (void)CFBridgingRelease(window->content_view);
        window->content_view = NULL;
    }
    if (window->ns_window) {
        (void)CFBridgingRelease(window->ns_window);
        window->ns_window = NULL;
    }

    free(window);
}

void ne_window_show(NEWindow *window) {
    if (!window) {
        return;
    }
    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    [ns_window makeKeyAndOrderFront:nil];
}

void ne_window_hide(NEWindow *window) {
    if (!window) {
        return;
    }
    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    [ns_window orderOut:nil];
}

void ne_window_request_close(NEWindow *window) {
    if (!window) {
        return;
    }
    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    /*
     * -performClose: simulates a click on the close button. Borderless windows
     * have no close button (no NSWindowStyleMaskClosable), so -performClose:
     * silently does nothing. Use -close directly, which still fires the
     * windowWillClose: delegate notification.
     */
    [ns_window close];
}

void ne_window_set_title(NEWindow *window, const char *title) {
    if (!window || !title) {
        return;
    }
    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    [ns_window setTitle:[NSString stringWithUTF8String:title]];
}

void ne_window_set_position(NEWindow *window, int32_t x, int32_t y) {
    if (!window) {
        return;
    }
    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    NSRect frame = [ns_window frame];
    frame.origin = NSMakePoint((CGFloat)x, (CGFloat)y);
    [ns_window setFrame:frame display:YES];
}

void ne_window_set_size(NEWindow *window, int32_t width, int32_t height) {
    if (!window || width <= 0 || height <= 0) {
        return;
    }
    NSWindow *ns_window = (__bridge NSWindow *)window->ns_window;
    /* Set the content (client) area size; the frame adjusts to fit decorations. */
    NSRect content_rect = [ns_window contentRectForFrameRect:[ns_window frame]];
    content_rect.size = NSMakeSize((CGFloat)width, (CGFloat)height);
    NSRect new_frame = [ns_window frameRectForContentRect:content_rect];
    [ns_window setFrame:new_frame display:YES];
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

    NSView *view = window->content_view ? (__bridge NSView *)window->content_view : nil;
    if (!view) {
        return false;
    }

    const NSRect bounds = [view bounds];
    if (out_width) {
        *out_width = (int32_t)bounds.size.width;
    }
    if (out_height) {
        *out_height = (int32_t)bounds.size.height;
    }

    return true;
}

bool ne_window_get_framebuffer_size(const NEWindow *window, int32_t *out_width, int32_t *out_height) {
    if (!window || !window->open) {
        return false;
    }

    NSView *view = window->content_view ? (__bridge NSView *)window->content_view : nil;
    if (!view) {
        return false;
    }

    const NSRect bounds = [view bounds];
    NSRect backing = bounds;

    if ([view respondsToSelector:@selector(convertRectToBacking:)]) {
        backing = [view convertRectToBacking:bounds];
    } else {
        NSWindow *ns_window = window->ns_window ? (__bridge NSWindow *)window->ns_window : nil;
        const CGFloat scale = ns_window ? [ns_window backingScaleFactor] : 1.0;
        backing.size.width = bounds.size.width * scale;
        backing.size.height = bounds.size.height * scale;
    }

    if (out_width) {
        *out_width = (int32_t)backing.size.width;
    }
    if (out_height) {
        *out_height = (int32_t)backing.size.height;
    }

    return true;
}

bool ne_window_get_content_scale(const NEWindow *window, float *out_scale) {
    if (!window || !window->open || !out_scale) {
        return false;
    }

    NSWindow *ns_window = window->ns_window ? (__bridge NSWindow *)window->ns_window : nil;
    if (!ns_window) {
        return false;
    }

    *out_scale = (float)[ns_window backingScaleFactor];
    return true;
}

void *ne_window_get_native_handle(NEWindow *window, NENativeHandleType type) {
    if (!window || !window->open) {
        return NULL;
    }

    switch (type) {
    case NE_NATIVE_HANDLE_COCOA_NS_WINDOW: {
        NSWindow *ns_window = window->ns_window ? (__bridge NSWindow *)window->ns_window : nil;
        return ns_window ? (__bridge void *)ns_window : NULL;
    }
    case NE_NATIVE_HANDLE_COCOA_NS_VIEW: {
        NSView *view = window->content_view ? (__bridge NSView *)window->content_view : nil;
        return view ? (__bridge void *)view : NULL;
    }

    /* Other handle types exist for cross-platform code but are unsupported on macOS builds. */
    case NE_NATIVE_HANDLE_WIN32_HWND:
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
