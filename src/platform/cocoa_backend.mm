// macOS Cocoa platform backend.
//
// One CocoaBackend instance per top-level window. A static CocoaApp
// (Objective-C side) lazily initializes NSApp, the application delegate,
// and the menu bar on first construction. The kqueue→runloop bridge that
// makes [NSApp nextEventMatchingMask:] wake up on fd readiness is set up
// independently in event_loop_kqueue.mm.

#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <CoreFoundation/CoreFoundation.h>
#import <OpenGL/gl3.h>
#import <Network/Network.h>

#include "platform/cocoa_backend.h"
#include "platform/keysym.h"
#include "core/types.h"
#include "core/debug.h"

#include <algorithm>
#include <string>
#include <vector>

@class RivtAppDelegate;
@class RivtView;
@class RivtWindow;

// =================================================================
// CocoaApp — process-wide singleton (NSApp, menu bar, key-window).
// =================================================================

namespace {

struct CocoaApp {
    static CocoaApp &instance();

    bool initialized = false;
    RivtAppDelegate *delegate = nil;
    rivt::CocoaBackend *key_backend = nullptr;
    NSOpenGLContext *shared_gl_context = nil;
    std::vector<rivt::CocoaBackend *> backends;

    void ensure_initialized();
    void build_menu();
    void attach(rivt::CocoaBackend *b);
    void detach(rivt::CocoaBackend *b);
};

CocoaApp &CocoaApp::instance() {
    static CocoaApp inst;
    return inst;
}

} // namespace

@interface RivtAppDelegate : NSObject <NSApplicationDelegate>
- (void)rivtNewWindow:(id)sender;
- (void)rivtCloseWindow:(id)sender;
@end

@implementation RivtAppDelegate
// Standard macOS behavior: closing the last window leaves the app
// running with just the menu bar visible. Cmd-Q (or App > Quit) is
// what actually terminates.
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return NO;
}

// Cmd-Q / App > Quit routes through our main loop so window destructors
// run (PTYs get SIGHUP, GL contexts release, etc.). We return Cancel so
// NSApp does not call exit() — instead the loop falls out of the while,
// main() returns, and the process exits normally.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    if (rivt::Platform::quit_handler()) rivt::Platform::quit_handler()();
    return NSTerminateCancel;
}

// Dock-icon click while no windows are visible reopens a fresh window.
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    if (!flag && rivt::Platform::new_window_handler())
        rivt::Platform::new_window_handler()();
    return YES;
}

- (void)rivtNewWindow:(id)sender {
    (void)sender;
    // Always use the global handler so Cmd-N works even when no window
    // is focused (which is the normal state once the user has closed
    // every window without quitting the app).
    if (rivt::Platform::new_window_handler())
        rivt::Platform::new_window_handler()();
}

- (void)rivtCloseWindow:(id)sender {
    auto *b = CocoaApp::instance().key_backend;
    if (b && b->on_menu_close_window) {
        b->on_menu_close_window();
    } else {
        [[NSApp keyWindow] performClose:sender];
    }
}
@end

namespace {

void CocoaApp::ensure_initialized() {
    if (initialized) return;
    initialized = true;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Launched from a terminal (not via LaunchServices), the Dock and
    // cmd-tab show the generic executable icon even though the bundle
    // has one — set it explicitly.
    NSString *icns = [[NSBundle mainBundle] pathForResource:@"rivt" ofType:@"icns"];
    if (icns) {
        NSImage *icon = [[NSImage alloc] initWithContentsOfFile:icns];
        if (icon) [NSApp setApplicationIconImage:icon];
    }

    delegate = [[RivtAppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    build_menu();

    // Sleep/wake + network-path changes drive the remote-link watchdog:
    // passive while the lid is closed, verify-or-reconnect immediately on
    // wake or when the route changes (wifi <-> tethering), instead of
    // waiting for the user to type into a dead session. Both callbacks
    // arrive on the main runloop, which our poll() pumps.
    NSNotificationCenter *nc = [[NSWorkspace sharedWorkspace] notificationCenter];
    [nc addObserverForName:NSWorkspaceWillSleepNotification object:nil queue:nil
        usingBlock:^(NSNotification *) {
        rivt::logmsg("rivt: system sleep — parking remote links\n");
        if (const auto &h = rivt::Platform::connectivity_handler()) h(false);
    }];
    [nc addObserverForName:NSWorkspaceDidWakeNotification object:nil queue:nil
        usingBlock:^(NSNotification *) {
        rivt::logmsg("rivt: system wake — verifying remote links\n");
        if (const auto &h = rivt::Platform::connectivity_handler()) h(true);
    }];
    nw_path_monitor_t mon = nw_path_monitor_create();
    nw_path_monitor_set_queue(mon, dispatch_get_main_queue());
    nw_path_monitor_set_update_handler(mon, ^(nw_path_t path) {
        bool up = nw_path_get_status(path) == nw_path_status_satisfied;
        // Log transitions only; path re-evaluations with the same status
        // are frequent and uninteresting.
        static int last = -1;
        if ((int)up != last) {
            last = (int)up;
            rivt::logmsg("rivt: network path %s\n", up ? "up" : "down");
        }
        if (const auto &h = rivt::Platform::connectivity_handler()) h(up);
    });
    nw_path_monitor_start(mon);

    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
}

void CocoaApp::build_menu() {
    NSMenu *menubar = [[NSMenu alloc] init];
    NSString *appName = [[NSProcessInfo processInfo] processName];

    // App menu
    {
        NSMenuItem *appItem = [[NSMenuItem alloc] init];
        [menubar addItem:appItem];
        NSMenu *appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"About %@", appName]
                           action:@selector(orderFrontStandardAboutPanel:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Hide %@", appName]
                           action:@selector(hide:)
                    keyEquivalent:@"h"];
        NSMenuItem *hideOthers = [appMenu addItemWithTitle:@"Hide Others"
                                                    action:@selector(hideOtherApplications:)
                                             keyEquivalent:@"h"];
        [hideOthers setKeyEquivalentModifierMask:(NSEventModifierFlagOption | NSEventModifierFlagCommand)];
        [appMenu addItemWithTitle:@"Show All"
                           action:@selector(unhideAllApplications:)
                    keyEquivalent:@""];
        [appMenu addItem:[NSMenuItem separatorItem]];
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        [appItem setSubmenu:appMenu];
    }

    // File menu
    {
        NSMenuItem *fileItem = [[NSMenuItem alloc] init];
        [menubar addItem:fileItem];
        NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        NSMenuItem *newWin = [fileMenu addItemWithTitle:@"New Window"
                                                 action:@selector(rivtNewWindow:)
                                          keyEquivalent:@"n"];
        [newWin setTarget:delegate];
        NSMenuItem *closeWin = [fileMenu addItemWithTitle:@"Close Window"
                                                   action:@selector(rivtCloseWindow:)
                                            keyEquivalent:@"w"];
        [closeWin setTarget:delegate];
        [fileItem setSubmenu:fileMenu];
    }

    // Edit menu
    {
        NSMenuItem *editItem = [[NSMenuItem alloc] init];
        [menubar addItem:editItem];
        NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        [editItem setSubmenu:editMenu];
    }

    // View menu
    {
        NSMenuItem *viewItem = [[NSMenuItem alloc] init];
        [menubar addItem:viewItem];
        NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
        NSMenuItem *fs = [viewMenu addItemWithTitle:@"Toggle Full Screen"
                                              action:@selector(toggleFullScreen:)
                                       keyEquivalent:@"f"];
        [fs setKeyEquivalentModifierMask:(NSEventModifierFlagControl | NSEventModifierFlagCommand)];
        [viewItem setSubmenu:viewMenu];
    }

    [NSApp setMainMenu:menubar];
}

void CocoaApp::attach(rivt::CocoaBackend *b) {
    backends.push_back(b);
    if (!key_backend) key_backend = b;
}

void CocoaApp::detach(rivt::CocoaBackend *b) {
    backends.erase(std::remove(backends.begin(), backends.end(), b), backends.end());
    if (key_backend == b) {
        key_backend = backends.empty() ? nullptr : backends.front();
    }
}

} // namespace

// =================================================================
// CocoaBackend::Impl
// =================================================================

namespace rivt {

struct CocoaBackend::Impl {
    RivtWindow *window = nil;
    RivtView *view = nil;
    NSOpenGLContext *gl_context = nil;
    int width = 0;
    int height = 0;
    float backing_scale = 1.0f;
    Platform::MouseCursor current_cursor = Platform::MouseCursor::Default;
    int cell_w = 0, cell_h = 0;
    int base_w = 0, base_h = 0;
    bool is_key_window = false;
    std::string clipboard_typed_data;
    std::string clipboard_typed_mime;
};

} // namespace rivt

// =================================================================
// Key translation: kVK_* virtual codes → XKB-style keysym values.
// =================================================================

namespace {

uint32_t translate_keycode(unsigned short kc, NSString *chars_no_mods) {
    // Named keys (function/navigation/modifiers) — kVK_* values
    switch (kc) {
        case 0x24: return XKB_KEY_Return;       // kVK_Return
        case 0x4C: return XKB_KEY_KP_Enter;     // kVK_ANSI_KeypadEnter
        case 0x30: return XKB_KEY_Tab;          // kVK_Tab
        case 0x31: return XKB_KEY_space;        // kVK_Space
        case 0x33: return XKB_KEY_BackSpace;    // kVK_Delete (i.e. backspace)
        case 0x35: return XKB_KEY_Escape;       // kVK_Escape
        case 0x75: return XKB_KEY_Delete;       // kVK_ForwardDelete
        case 0x73: return XKB_KEY_Home;         // kVK_Home
        case 0x77: return XKB_KEY_End;          // kVK_End
        case 0x74: return XKB_KEY_Page_Up;      // kVK_PageUp
        case 0x79: return XKB_KEY_Page_Down;    // kVK_PageDown
        case 0x7B: return XKB_KEY_Left;         // kVK_LeftArrow
        case 0x7C: return XKB_KEY_Right;        // kVK_RightArrow
        case 0x7D: return XKB_KEY_Down;         // kVK_DownArrow
        case 0x7E: return XKB_KEY_Up;           // kVK_UpArrow
        case 0x7A: return XKB_KEY_F1;
        case 0x78: return XKB_KEY_F2;
        case 0x63: return XKB_KEY_F3;
        case 0x76: return XKB_KEY_F4;
        case 0x60: return XKB_KEY_F5;
        case 0x61: return XKB_KEY_F6;
        case 0x62: return XKB_KEY_F7;
        case 0x64: return XKB_KEY_F8;
        case 0x65: return XKB_KEY_F9;
        case 0x6D: return XKB_KEY_F10;
        case 0x67: return XKB_KEY_F11;
        case 0x6F: return XKB_KEY_F12;
        case 0x39: return XKB_KEY_Caps_Lock;    // kVK_CapsLock
        case 0x6B: return XKB_KEY_Scroll_Lock;  // kVK_F14 (no real ScrollLock on Mac)
        case 0x47: return XKB_KEY_Num_Lock;     // kVK_ANSI_KeypadClear
        case 0x69: return XKB_KEY_Print;        // kVK_F13
        case 0x71: return XKB_KEY_Pause;        // kVK_F15
        default: break;
    }
    if (chars_no_mods.length > 0) {
        unichar c = [chars_no_mods characterAtIndex:0];
        if (c >= 0x20 && c <= 0x7E) {
            // ASCII range: keysym value matches codepoint.
            return c;
        }
        return c;
    }
    return 0;
}

rivt::KeyMod translate_mods(NSEventModifierFlags flags) {
    using rivt::KeyMod;
    KeyMod m = KeyMod::NoMod;
    if (flags & NSEventModifierFlagShift)   m = m | KeyMod::Shift;
    if (flags & NSEventModifierFlagControl) m = m | KeyMod::Ctrl;
    if (flags & NSEventModifierFlagOption)  m = m | KeyMod::Alt;
    if (flags & NSEventModifierFlagCommand) m = m | KeyMod::Super;
    return m;
}

uint32_t modifier_keysym_from_keycode(unsigned short kc) {
    switch (kc) {
        case 0x38: return XKB_KEY_Shift_L;       // kVK_Shift
        case 0x3C: return XKB_KEY_Shift_R;       // kVK_RightShift
        case 0x3B: return XKB_KEY_Control_L;     // kVK_Control
        case 0x3E: return XKB_KEY_Control_R;     // kVK_RightControl
        case 0x3A: return XKB_KEY_Alt_L;         // kVK_Option
        case 0x3D: return XKB_KEY_Alt_R;         // kVK_RightOption
        case 0x37: return XKB_KEY_Super_L;       // kVK_Command
        case 0x36: return XKB_KEY_Super_R;       // kVK_RightCommand
        default: return 0;
    }
}

} // namespace

// =================================================================
// RivtWindow (NSWindow subclass — needs canBecomeKeyWindow override
// for borderless options later, plus simpler delegate routing).
// =================================================================

@interface RivtWindow : NSWindow
@property(nonatomic, assign) rivt::CocoaBackend *backend;
@end

@implementation RivtWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

// =================================================================
// RivtView — the NSView that hosts the GL surface and receives input.
// =================================================================

@interface RivtView : NSView <NSWindowDelegate> {
    rivt::CocoaBackend *_backend;
    NSTrackingArea *_tracking_area;
    rivt::MouseButton _drag_button;
    bool _dragging;
}
- (instancetype)initWithBackend:(rivt::CocoaBackend *)backend;
- (void)clearBackend;
- (void)dispatchKey:(NSEvent *)event pressed:(BOOL)pressed;
- (void)dispatchMouse:(NSEvent *)event button:(rivt::MouseButton)button
              pressed:(BOOL)pressed motion:(BOOL)motion;
// Standard responder-chain actions wired to the Edit menu (Cmd-C / Cmd-V).
- (void)copy:(id)sender;
- (void)paste:(id)sender;
@end

@implementation RivtView

- (instancetype)initWithBackend:(rivt::CocoaBackend *)backend {
    self = [super initWithFrame:NSMakeRect(0, 0, 800, 600)];
    if (self) {
        _backend = backend;
        _drag_button = rivt::MouseButton::NoButton;
        _dragging = false;
        self.wantsLayer = YES;
    }
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }   // top-left origin, matches GL viewport math
- (BOOL)isOpaque { return YES; }

- (void)clearBackend {
    _backend = nullptr;
}

- (void)copy:(id)sender {
    (void)sender;
    if (_backend && _backend->on_menu_copy) _backend->on_menu_copy();
}

- (void)paste:(id)sender {
    (void)sender;
    if (_backend && _backend->on_menu_paste) _backend->on_menu_paste();
}

// Validate menu items that target the responder chain. Without this, AppKit
// can't tell the action is implemented and the menu item stays disabled,
// which makes the Cmd-C / Cmd-V key equivalents fall through to keyDown:.
- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item {
    SEL action = [item action];
    if (action == @selector(copy:) || action == @selector(paste:) ||
        action == @selector(selectAll:)) {
        return _backend != nullptr;
    }
    return YES;
}

- (void)updateTrackingAreas {
    if (_tracking_area) {
        [self removeTrackingArea:_tracking_area];
        _tracking_area = nil;
    }
    NSTrackingAreaOptions opts = (NSTrackingActiveAlways | NSTrackingMouseMoved |
                                  NSTrackingInVisibleRect | NSTrackingCursorUpdate);
    _tracking_area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                                  options:opts
                                                    owner:self
                                                 userInfo:nil];
    [self addTrackingArea:_tracking_area];
    [super updateTrackingAreas];
}

- (void)cursorUpdate:(NSEvent *)event {
    (void)event;
    NSCursor *c = [NSCursor arrowCursor];
    switch (_backend ? _backend->_impl_cursor() : rivt::Platform::MouseCursor::Default) {
        case rivt::Platform::MouseCursor::Hand: c = [NSCursor pointingHandCursor]; break;
        case rivt::Platform::MouseCursor::Text: c = [NSCursor IBeamCursor]; break;
        default: break;
    }
    [c set];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (!_backend) return;
    NSSize backing = [self convertSizeToBacking:newSize];
    int w = (int)backing.width;
    int h = (int)backing.height;
    _backend->_impl_set_size(w, h);
    // Resize the GL drawable to match the new view size. Without this,
    // NSOpenGLContext keeps its original drawable size and content gets
    // clipped at the bottom-left corner.
    _backend->_impl_update_gl_drawable();
    if (_backend->on_resize) _backend->on_resize(w, h);
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (!_backend) return;
    float scale = (float)[[self window] backingScaleFactor];
    _backend->_impl_set_scale(scale);
    NSSize backing = [self convertSizeToBacking:[self bounds].size];
    int w = (int)backing.width;
    int h = (int)backing.height;
    _backend->_impl_set_size(w, h);
    _backend->_impl_update_gl_drawable();
    if (_backend->on_resize) _backend->on_resize(w, h);
}

// ---------------- Window delegate ----------------

- (BOOL)windowShouldClose:(NSWindow *)sender {
    (void)sender;
    if (_backend && _backend->on_close) _backend->on_close();
    return NO;  // app code handles teardown via mark_closing()
}

- (void)windowDidBecomeKey:(NSNotification *)note {
    (void)note;
    if (!_backend) return;
    CocoaApp::instance().key_backend = _backend;
    _backend->notify_key_window_focus(true);
    if (_backend->on_focus) _backend->on_focus(true);
}

- (void)windowDidResignKey:(NSNotification *)note {
    (void)note;
    if (!_backend) return;
    _backend->notify_key_window_focus(false);
    if (_backend->on_focus) _backend->on_focus(false);
}

// ---------------- Keyboard ----------------

- (void)keyDown:(NSEvent *)event   { [self dispatchKey:event pressed:YES]; }
- (void)keyUp:(NSEvent *)event     { [self dispatchKey:event pressed:NO];  }

- (void)flagsChanged:(NSEvent *)event {
    if (!_backend || !_backend->on_key) return;
    uint32_t sym = modifier_keysym_from_keycode([event keyCode]);
    if (sym == 0) return;
    NSEventModifierFlags flags = [event modifierFlags];
    // Decide press/release based on whether the corresponding mask is now set.
    bool pressed = false;
    switch (sym) {
        case XKB_KEY_Shift_L:
        case XKB_KEY_Shift_R:    pressed = (flags & NSEventModifierFlagShift)   != 0; break;
        case XKB_KEY_Control_L:
        case XKB_KEY_Control_R:  pressed = (flags & NSEventModifierFlagControl) != 0; break;
        case XKB_KEY_Alt_L:
        case XKB_KEY_Alt_R:      pressed = (flags & NSEventModifierFlagOption)  != 0; break;
        case XKB_KEY_Super_L:
        case XKB_KEY_Super_R:    pressed = (flags & NSEventModifierFlagCommand) != 0; break;
        default: break;
    }
    rivt::KeyEvent ke{};
    ke.keysym = sym;
    ke.mods = translate_mods(flags);
    ke.text = "";
    ke.pressed = pressed;
    _backend->on_key(ke);
}

- (void)dispatchKey:(NSEvent *)event pressed:(BOOL)pressed {
    if (!_backend || !_backend->on_key) return;
    NSString *charsNoMods = [event charactersIgnoringModifiers];
    rivt::KeyEvent ke{};
    ke.keysym = translate_keycode([event keyCode], charsNoMods);
    ke.mods = translate_mods([event modifierFlags]);
    NSString *chars = [event characters];
    if (chars.length > 0) {
        ke.text = std::string([chars UTF8String]);
    }
    ke.pressed = pressed ? true : false;
    _backend->on_key(ke);
}

// ---------------- Mouse ----------------

- (void)mouseDown:(NSEvent *)event {
    _drag_button = rivt::MouseButton::Left;
    _dragging = true;
    [self dispatchMouse:event button:rivt::MouseButton::Left pressed:YES motion:NO];
}
- (void)mouseUp:(NSEvent *)event {
    [self dispatchMouse:event button:rivt::MouseButton::Left pressed:NO motion:NO];
    _drag_button = rivt::MouseButton::NoButton;
    _dragging = false;
}
- (void)mouseDragged:(NSEvent *)event {
    [self dispatchMouse:event button:_drag_button pressed:YES motion:YES];
}
- (void)rightMouseDown:(NSEvent *)event {
    _drag_button = rivt::MouseButton::Right;
    _dragging = true;
    [self dispatchMouse:event button:rivt::MouseButton::Right pressed:YES motion:NO];
}
- (void)rightMouseUp:(NSEvent *)event {
    [self dispatchMouse:event button:rivt::MouseButton::Right pressed:NO motion:NO];
    _drag_button = rivt::MouseButton::NoButton;
    _dragging = false;
}
- (void)rightMouseDragged:(NSEvent *)event {
    [self dispatchMouse:event button:rivt::MouseButton::Right pressed:YES motion:YES];
}
- (void)otherMouseDown:(NSEvent *)event {
    _drag_button = rivt::MouseButton::Middle;
    _dragging = true;
    [self dispatchMouse:event button:rivt::MouseButton::Middle pressed:YES motion:NO];
}
- (void)otherMouseUp:(NSEvent *)event {
    [self dispatchMouse:event button:rivt::MouseButton::Middle pressed:NO motion:NO];
    _drag_button = rivt::MouseButton::NoButton;
    _dragging = false;
}
- (void)otherMouseDragged:(NSEvent *)event {
    [self dispatchMouse:event button:_drag_button pressed:YES motion:YES];
}
- (void)mouseMoved:(NSEvent *)event {
    [self dispatchMouse:event button:rivt::MouseButton::NoButton pressed:NO motion:YES];
}
- (void)scrollWheel:(NSEvent *)event {
    if (!_backend || !_backend->on_mouse) return;
    CGFloat dy = [event scrollingDeltaY];
    if (dy == 0) return;
    NSPoint w = [event locationInWindow];
    NSRect viewFrameInWindow = [self convertRect:[self bounds] toView:nil];
    CGFloat local_x = w.x - NSMinX(viewFrameInWindow);
    CGFloat local_y = NSMaxY(viewFrameInWindow) - w.y;
    CGFloat scale = [[self window] backingScaleFactor];
    rivt::MouseEvent me{};
    me.x = (int)(local_x * scale);
    me.y = (int)(local_y * scale);
    me.button = (dy > 0) ? rivt::MouseButton::ScrollUp : rivt::MouseButton::ScrollDown;
    me.mods = translate_mods([event modifierFlags]);
    me.pressed = true;
    me.motion = false;
    _backend->on_mouse(me);
}

- (void)dispatchMouse:(NSEvent *)event button:(rivt::MouseButton)button
              pressed:(BOOL)pressed motion:(BOOL)motion {
    if (!_backend || !_backend->on_mouse) return;
    // Compute view-local coords manually rather than going through
    // convertPointToBacking: (which has subtle interactions with
    // isFlipped views). locationInWindow is in window base coords,
    // origin at the window content's bottom-left, Y-up.
    NSPoint w = [event locationInWindow];
    NSRect viewFrameInWindow = [self convertRect:[self bounds] toView:nil];
    CGFloat local_x = w.x - NSMinX(viewFrameInWindow);
    CGFloat local_y = NSMaxY(viewFrameInWindow) - w.y;  // Y-down from view top
    CGFloat scale = [[self window] backingScaleFactor];
    rivt::MouseEvent me{};
    me.x = (int)(local_x * scale);
    me.y = (int)(local_y * scale);
    me.button = button;
    me.mods = translate_mods([event modifierFlags]);
    me.pressed = pressed ? true : false;
    me.motion = motion ? true : false;
    _backend->on_mouse(me);
}

// Cocoa's default key handling routes Command-key shortcuts directly to
// menu items, swallowing them. We do nothing extra here — the menu items
// for Cmd-N/W/Q reach the app delegate and the rest of the keys get
// keyDown: as normal.

@end

// =================================================================
// CocoaBackend implementation
// =================================================================

namespace rivt {

CocoaBackend::CocoaBackend() : m_impl(new Impl()) {
    CocoaApp::instance().ensure_initialized();
    CocoaApp::instance().attach(this);
}

CocoaBackend::~CocoaBackend() {
    CocoaApp::instance().detach(this);
    // Detach delegate callbacks before tearing down the NSWindow — closing
    // the window fires windowWillClose: synchronously and we must not
    // re-enter this dying backend.
    if (m_impl->view) {
        [m_impl->view clearBackend];
        if (m_impl->window) {
            [m_impl->window setDelegate:nil];
        }
    }
    if (m_impl->window) {
        [m_impl->window close];
        m_impl->window = nil;
    }
    m_impl->view = nil;
    m_impl->gl_context = nil;
    delete m_impl;
}

void CocoaBackend::_impl_set_size(int w, int h) {
    m_impl->width = w;
    m_impl->height = h;
}
void CocoaBackend::_impl_set_scale(float s) {
    m_impl->backing_scale = s;
}
Platform::MouseCursor CocoaBackend::_impl_cursor() const {
    return m_impl->current_cursor;
}

bool CocoaBackend::create_window(int width, int height, const std::string &title) {
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindowStyleMask style = (NSWindowStyleMaskTitled |
                               NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable |
                               NSWindowStyleMaskResizable);
    RivtWindow *win = [[RivtWindow alloc] initWithContentRect:frame
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    [win setReleasedWhenClosed:NO];
    [win setTitle:[NSString stringWithUTF8String:title.c_str()]];
    win.backend = this;

    RivtView *view = [[RivtView alloc] initWithBackend:this];
    [win setContentView:view];
    [win setDelegate:view];
    [win makeFirstResponder:view];
    [win center];

    m_impl->window = win;
    m_impl->view = view;
    m_impl->backing_scale = (float)[win backingScaleFactor];
    NSSize backing = [view convertSizeToBacking:[view bounds].size];
    m_impl->width = (int)backing.width;
    m_impl->height = (int)backing.height;
    return true;
}

void CocoaBackend::destroy_window() {
    if (m_impl->window) {
        [m_impl->window close];
        m_impl->window = nil;
    }
    m_impl->view = nil;
}

void CocoaBackend::set_title(const std::string &title) {
    if (m_impl->window)
        [m_impl->window setTitle:[NSString stringWithUTF8String:title.c_str()]];
}

void CocoaBackend::get_size(int &width, int &height) {
    width = m_impl->width;
    height = m_impl->height;
}

void CocoaBackend::resize_window(int width, int height) {
    if (!m_impl->window) return;
    // width/height are pixels; convert to points for setContentSize:
    CGFloat s = m_impl->backing_scale > 0 ? m_impl->backing_scale : 1.0f;
    NSSize pts = NSMakeSize(width / s, height / s);
    [m_impl->window setContentSize:pts];
}

void CocoaBackend::show_window() {
    if (m_impl->window) {
        [m_impl->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void CocoaBackend::set_size_hints(int cell_w, int cell_h, int base_w, int base_h) {
    m_impl->cell_w = cell_w;
    m_impl->cell_h = cell_h;
    m_impl->base_w = base_w;
    m_impl->base_h = base_h;
    if (!m_impl->window) return;
    CGFloat s = m_impl->backing_scale > 0 ? m_impl->backing_scale : 1.0f;
    [m_impl->window setContentResizeIncrements:NSMakeSize(cell_w / s, cell_h / s)];
    [m_impl->window setContentMinSize:NSMakeSize(base_w / s + cell_w / s,
                                                 base_h / s + cell_h / s)];
}

bool CocoaBackend::create_gl_context() {
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24,
        0
    };
    NSOpenGLPixelFormat *fmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!fmt) {
        dbg("cocoa: NSOpenGLPixelFormat init failed");
        return false;
    }
    NSOpenGLContext *share = CocoaApp::instance().shared_gl_context;
    NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:fmt
                                                       shareContext:share];
    if (!ctx) {
        dbg("cocoa: NSOpenGLContext init failed");
        return false;
    }
    // The view's GL surface must use backing (Retina) pixels so the
    // drawable size matches what we pass to the renderer (which works
    // in backing pixels for crisp text on HiDPI displays).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [m_impl->view setWantsBestResolutionOpenGLSurface:YES];
#pragma clang diagnostic pop

    // NSOpenGLContext.setView is API_DEPRECATED on 10.14+ in favor of
    // NSOpenGLView, but the underlying mechanism still works and matches
    // the platform-neutral renderer's expectation of an explicit
    // make_current/swap_buffers pair. Suppress the deprecation warning.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [ctx setView:m_impl->view];
#pragma clang diagnostic pop
    [ctx update];
    if (!share) CocoaApp::instance().shared_gl_context = ctx;
    m_impl->gl_context = ctx;
    [ctx makeCurrentContext];
    return true;
}

void CocoaBackend::_impl_update_gl_drawable() {
    if (m_impl->gl_context) [m_impl->gl_context update];
}

void CocoaBackend::make_current() {
    if (m_impl->gl_context) [m_impl->gl_context makeCurrentContext];
}

void CocoaBackend::swap_buffers() {
    if (m_impl->gl_context) [m_impl->gl_context flushBuffer];
}

// ---------------- Clipboard ----------------

void CocoaBackend::set_clipboard(const std::string &text, bool /*primary*/) {
    // macOS has no primary-selection concept — both flavors map to general.
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    NSString *s = [NSString stringWithUTF8String:text.c_str()];
    if (s) [pb setString:s forType:NSPasteboardTypeString];
}

std::string CocoaBackend::get_clipboard(bool /*primary*/) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s = [pb stringForType:NSPasteboardTypeString];
    if (!s) return {};
    return std::string([s UTF8String]);
}

void CocoaBackend::set_clipboard_data(const std::string &data,
                                      const std::string &mime_type,
                                      bool primary) {
    if (mime_type.empty() || mime_type == "text/plain") {
        set_clipboard(data, primary);
        return;
    }
    // Best-effort: stash typed data and write a binary type to the pasteboard
    // for the very limited cases that need it (e.g. Kitty PNG).
    m_impl->clipboard_typed_data = data;
    m_impl->clipboard_typed_mime = mime_type;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    NSString *uti = nil;
    if (mime_type == "image/png") uti = @"public.png";
    if (uti) {
        NSData *d = [NSData dataWithBytes:data.data() length:data.size()];
        [pb setData:d forType:uti];
    }
}

std::string CocoaBackend::get_clipboard_data(const std::string &mime_type,
                                             bool primary) {
    if (mime_type.empty() || mime_type == "text/plain")
        return get_clipboard(primary);
    if (m_impl->clipboard_typed_mime == mime_type)
        return m_impl->clipboard_typed_data;
    return {};
}

// ---------------- DPI & cursor ----------------

float CocoaBackend::get_dpi_scale() {
    return m_impl->backing_scale > 0 ? m_impl->backing_scale : 1.0f;
}

void CocoaBackend::set_mouse_cursor(MouseCursor cursor) {
    if (m_impl->current_cursor == cursor) return;
    m_impl->current_cursor = cursor;
    if (m_impl->window) {
        [[m_impl->window contentView] performSelector:@selector(cursorUpdate:)
                                           withObject:nil];
    }
}

void CocoaBackend::notify_key_window_focus(bool focused) {
    m_impl->is_key_window = focused;
}

} // namespace rivt
