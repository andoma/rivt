#include "platform/x11_backend.h"
#include "core/debug.h"
#include <xcb/xcb_icccm.h>
// xcb/xkb.h uses 'explicit' as a field name which is a C++ keyword
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <unistd.h>

// EGL needs the X11 display for its native display type
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

namespace rivt {

// A selection owner that never answers must not wedge paste forever, and a
// hostile or buggy one must not make us allocate without bound.
static constexpr auto kPasteTimeout = std::chrono::seconds(2);
static constexpr size_t kMaxPasteBytes = 64 * 1024 * 1024;

X11Backend::X11Backend() {
    m_xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_xkb_ctx)
        throw std::runtime_error("Failed to create xkb context");
}

X11Backend::~X11Backend() {
    destroy_window();
    if (m_xkb_compose_state) xkb_compose_state_unref(m_xkb_compose_state);
    if (m_xkb_compose_table) xkb_compose_table_unref(m_xkb_compose_table);
    if (m_xkb_state) xkb_state_unref(m_xkb_state);
    if (m_xkb_keymap) xkb_keymap_unref(m_xkb_keymap);
    if (m_xkb_ctx) xkb_context_unref(m_xkb_ctx);
}

xcb_atom_t X11Backend::intern_atom(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(m_conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(m_conn, cookie, nullptr);
    xcb_atom_t atom = reply ? reply->atom : (xcb_atom_t)XCB_ATOM_NONE;
    free(reply);
    return atom;
}

bool X11Backend::create_window(int width, int height, const std::string &title) {
    // Use Xlib to get both Display* (for EGL) and xcb connection
    m_xlib_display = XOpenDisplay(nullptr);
    if (!m_xlib_display) return false;

    m_conn = XGetXCBConnection(m_xlib_display);
    if (!m_conn || xcb_connection_has_error(m_conn)) return false;

    // Let XCB own the event queue
    XSetEventQueueOwner(m_xlib_display, XCBOwnsEventQueue);

    m_screen = xcb_setup_roots_iterator(xcb_get_setup(m_conn)).data;

    m_window = xcb_generate_id(m_conn);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        m_screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_FOCUS_CHANGE |
        XCB_EVENT_MASK_PROPERTY_CHANGE
    };

    xcb_create_window(m_conn, XCB_COPY_FROM_PARENT, m_window, m_screen->root,
                      0, 0, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      m_screen->root_visual, mask, values);

    m_width = width;
    m_height = height;

    // Intern atoms
    m_atom_clipboard = intern_atom("CLIPBOARD");
    m_atom_utf8_string = intern_atom("UTF8_STRING");
    m_atom_targets = intern_atom("TARGETS");
    m_atom_rivt_sel = intern_atom("RIVT_SELECTION");
    m_atom_wm_protocols = intern_atom("WM_PROTOCOLS");
    m_atom_wm_delete = intern_atom("WM_DELETE_WINDOW");
    m_atom_image_png = intern_atom("image/png");
    m_atom_incr = intern_atom("INCR");

    // Register for WM_DELETE_WINDOW
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_window,
                        m_atom_wm_protocols, XCB_ATOM_ATOM, 32, 1, &m_atom_wm_delete);

    // Set WM_CLASS so the desktop environment can group windows and match icons
    // Format: instance_name\0class_name\0
    const char wm_class[] = "rivt\0rivt";
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_window,
                        XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(wm_class), wm_class);

    // Set _NET_WM_PID for taskbar integration
    uint32_t pid = getpid();
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_window,
                        intern_atom("_NET_WM_PID"), XCB_ATOM_CARDINAL, 32,
                        1, &pid);

    set_title(title);
    create_cursors();
    xcb_flush(m_conn);

    // Setup xkbcommon-x11
    {
        uint8_t xkb_base_event, xkb_base_error;
        xkb_x11_setup_xkb_extension(m_conn,
            XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr,
            &xkb_base_event, &xkb_base_error);
        m_xkb_first_event = xkb_base_event;

        // Subscribe to XKB events: keymap changes and state changes
        uint16_t events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY
                        | XCB_XKB_EVENT_TYPE_MAP_NOTIFY
                        | XCB_XKB_EVENT_TYPE_STATE_NOTIFY;
        xcb_xkb_select_events(m_conn, XCB_XKB_ID_USE_CORE_KBD,
                              events, 0, events, 0, 0, nullptr);
    }

    m_xkb_device_id = xkb_x11_get_core_keyboard_device_id(m_conn);
    m_xkb_keymap = xkb_x11_keymap_new_from_device(m_xkb_ctx, m_conn, m_xkb_device_id,
                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    m_xkb_state = xkb_x11_state_new_from_device(m_xkb_keymap, m_conn, m_xkb_device_id);

    // Setup compose table from locale (for dead keys / compose sequences)
    const char *locale = getenv("LC_ALL");
    if (!locale || !*locale) locale = getenv("LC_CTYPE");
    if (!locale || !*locale) locale = getenv("LANG");
    if (!locale || !*locale) locale = "C";
    m_xkb_compose_table = xkb_compose_table_new_from_locale(
        m_xkb_ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (m_xkb_compose_table) {
        m_xkb_compose_state = xkb_compose_state_new(
            m_xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    }

    m_key_symbols = xcb_key_symbols_alloc(m_conn);

    // EGL setup
    m_egl_display = eglGetDisplay((EGLNativeDisplayType)m_xlib_display);
    if (m_egl_display == EGL_NO_DISPLAY) return false;

    EGLint major, minor;
    if (!eglInitialize(m_egl_display, &major, &minor)) return false;

    return true;
}

bool X11Backend::create_gl_context() {
    eglBindAPI(EGL_OPENGL_API);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(m_egl_display, config_attribs, &config, 1, &num_configs) || num_configs == 0)
        return false;

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };

    m_egl_context = eglCreateContext(m_egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (m_egl_context == EGL_NO_CONTEXT) return false;

    m_egl_surface = eglCreateWindowSurface(m_egl_display, config,
                                           (EGLNativeWindowType)m_window, nullptr);
    if (m_egl_surface == EGL_NO_SURFACE) return false;

    eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context);
    eglSwapInterval(m_egl_display, 1);
    return true;
}

void X11Backend::make_current() {
    eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context);
}

void X11Backend::swap_buffers() {
    eglSwapBuffers(m_egl_display, m_egl_surface);
}

void X11Backend::destroy_window() {
    if (m_egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (m_egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_egl_display, m_egl_surface);
        m_egl_surface = EGL_NO_SURFACE;
    }
    if (m_egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_egl_display, m_egl_context);
        m_egl_context = EGL_NO_CONTEXT;
    }
    if (m_egl_display != EGL_NO_DISPLAY) {
        eglTerminate(m_egl_display);
        m_egl_display = EGL_NO_DISPLAY;
    }
    eglReleaseThread();
    if (m_key_symbols) {
        xcb_key_symbols_free(m_key_symbols);
        m_key_symbols = nullptr;
    }
    if (m_window && m_conn) {
        xcb_destroy_window(m_conn, m_window);
        m_window = 0;
    }
    // XCloseDisplay closes the underlying xcb connection too
    if (m_xlib_display) {
        XCloseDisplay(m_xlib_display);
        m_xlib_display = nullptr;
        m_conn = nullptr;
    }
}

void X11Backend::set_title(const std::string &title) {
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_window,
                        XCB_ATOM_WM_NAME, m_atom_utf8_string, 8,
                        title.size(), title.c_str());
    xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, m_window,
                        intern_atom("_NET_WM_NAME"), m_atom_utf8_string, 8,
                        title.size(), title.c_str());
    xcb_flush(m_conn);
}

void X11Backend::get_size(int &width, int &height) {
    width = m_width;
    height = m_height;
}

void X11Backend::resize_window(int width, int height) {
    uint32_t values[] = { (uint32_t)width, (uint32_t)height };
    xcb_configure_window(m_conn, m_window,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
    xcb_flush(m_conn);
    m_width = width;
    m_height = height;
}

void X11Backend::show_window() {
    xcb_map_window(m_conn, m_window);
    xcb_flush(m_conn);
}

void X11Backend::set_size_hints(int cell_w, int cell_h, int base_w, int base_h) {
    xcb_size_hints_t hints{};
    xcb_icccm_size_hints_set_resize_inc(&hints, cell_w, cell_h);
    xcb_icccm_size_hints_set_base_size(&hints, base_w, base_h);
    xcb_icccm_set_wm_normal_hints(m_conn, m_window, &hints);
    xcb_flush(m_conn);
}

int X11Backend::get_event_fd() {
    return xcb_get_file_descriptor(m_conn);
}

KeyMod X11Backend::translate_mods(uint16_t state) {
    KeyMod mods = KeyMod::NoMod;
    if (state & XCB_MOD_MASK_SHIFT)   mods = mods | KeyMod::Shift;
    if (state & XCB_MOD_MASK_CONTROL) mods = mods | KeyMod::Ctrl;
    if (state & XCB_MOD_MASK_1)       mods = mods | KeyMod::Alt;
    if (state & XCB_MOD_MASK_4)       mods = mods | KeyMod::Super;
    return mods;
}

void X11Backend::handle_key_event(xcb_key_press_event_t *ev, bool pressed) {
    xkb_state_update_key(m_xkb_state, ev->detail, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    KeyEvent key{};
    key.pressed = pressed;
    key.mods = translate_mods(ev->state);

    // Get the keysym with Shift applied but without Ctrl, so we see 'c'/'C' not a
    // control character. This lets Ctrl+C map to ETX and Ctrl+Shift+C match XKB_KEY_C.
    {
        const xkb_keysym_t *syms;
        xkb_layout_index_t layout = xkb_state_key_get_layout(m_xkb_state, ev->detail);
        // level 0 = unshifted, level 1 = shifted
        int level = (ev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0;
        int n = xkb_keymap_key_get_syms_by_level(m_xkb_keymap, ev->detail, layout, level, &syms);
        key.keysym = (n > 0) ? syms[0] : xkb_state_key_get_one_sym(m_xkb_state, ev->detail);
    }

    if (pressed) {
        // Feed keysym into compose state for dead key / compose sequence handling
        if (m_xkb_compose_state) {
            xkb_keysym_t raw_sym = xkb_state_key_get_one_sym(m_xkb_state, ev->detail);
            xkb_compose_state_feed(m_xkb_compose_state, raw_sym);
            enum xkb_compose_status status = xkb_compose_state_get_status(m_xkb_compose_state);

            if (status == XKB_COMPOSE_COMPOSING) {
                // In the middle of a compose sequence — swallow the event
                return;
            } else if (status == XKB_COMPOSE_COMPOSED) {
                // Compose sequence complete — use composed keysym and text
                key.keysym = xkb_compose_state_get_one_sym(m_xkb_compose_state);
                char buf[64];
                int len = xkb_compose_state_get_utf8(m_xkb_compose_state, buf, sizeof(buf));
                if (len > 0 && (unsigned char)buf[0] >= 0x20)
                    key.text = std::string(buf, len);
                xkb_compose_state_reset(m_xkb_compose_state);
                if (on_key) on_key(key);
                return;
            } else if (status == XKB_COMPOSE_CANCELLED) {
                xkb_compose_state_reset(m_xkb_compose_state);
                // Fall through to normal handling
            }
            // XKB_COMPOSE_NOTHING — fall through to normal handling
        }

        char buf[64];
        int len = xkb_state_key_get_utf8(m_xkb_state, ev->detail, buf, sizeof(buf));
        if (len > 0 && (unsigned char)buf[0] >= 0x20) {
            key.text = std::string(buf, len);
        }
        // When Ctrl is held, xkb produces control chars (< 0x20) with no text.
        // Provide the unmodified letter as text so encode_key can map Ctrl+letter.
        if (key.text.empty() && (key.mods & KeyMod::Ctrl)) {
            xkb_keysym_t sym = key.keysym;
            if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) {
                key.text = std::string(1, (char)('a' + (sym - XKB_KEY_a)));
            } else if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
                key.text = std::string(1, (char)('A' + (sym - XKB_KEY_A)));
            }
        }
    }

    if (on_key) on_key(key);
}

void X11Backend::handle_button_event(xcb_button_press_event_t *ev, bool pressed) {
    MouseEvent mouse{};
    mouse.x = ev->event_x;
    mouse.y = ev->event_y;
    mouse.pressed = pressed;
    mouse.motion = false;
    mouse.mods = translate_mods(ev->state);

    switch (ev->detail) {
        case 1: mouse.button = MouseButton::Left; break;
        case 2: mouse.button = MouseButton::Middle; break;
        case 3: mouse.button = MouseButton::Right; break;
        case 4: mouse.button = MouseButton::ScrollUp; break;
        case 5: mouse.button = MouseButton::ScrollDown; break;
        default: return;
    }

    // Grab/ungrab pointer so we receive the release even if the cursor
    // leaves the window (needed for drag-select).
    if (mouse.button == MouseButton::Left) {
        if (pressed) {
            xcb_grab_pointer(m_conn, 0, m_window,
                             XCB_EVENT_MASK_BUTTON_RELEASE |
                             XCB_EVENT_MASK_POINTER_MOTION,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                             XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
            xcb_flush(m_conn);
        } else {
            xcb_ungrab_pointer(m_conn, XCB_CURRENT_TIME);
            xcb_flush(m_conn);
        }
    }

    if (on_mouse) on_mouse(mouse);
}

void X11Backend::handle_motion_event(xcb_motion_notify_event_t *ev) {
    MouseEvent mouse{};
    mouse.x = ev->event_x;
    mouse.y = ev->event_y;
    mouse.button = MouseButton::NoButton;
    mouse.pressed = false;
    mouse.motion = true;
    mouse.mods = translate_mods(ev->state);

    // Detect held buttons from state
    if (ev->state & XCB_BUTTON_MASK_1) mouse.button = MouseButton::Left;
    else if (ev->state & XCB_BUTTON_MASK_2) mouse.button = MouseButton::Middle;
    else if (ev->state & XCB_BUTTON_MASK_3) mouse.button = MouseButton::Right;

    if (on_mouse) on_mouse(mouse);
}

void X11Backend::handle_configure_event(xcb_configure_notify_event_t *ev) {
    if (ev->width != m_width || ev->height != m_height) {
        m_width = ev->width;
        m_height = ev->height;
        if (on_resize) on_resize(m_width, m_height);
    }
}

void X11Backend::process_events() {
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(m_conn)) != nullptr) {
        uint8_t type = ev->response_type & ~0x80;
        switch (type) {
            case XCB_KEY_PRESS:
                handle_key_event((xcb_key_press_event_t *)ev, true);
                break;
            case XCB_KEY_RELEASE:
                handle_key_event((xcb_key_press_event_t *)ev, false);
                break;
            case XCB_BUTTON_PRESS:
                handle_button_event((xcb_button_press_event_t *)ev, true);
                break;
            case XCB_BUTTON_RELEASE:
                handle_button_event((xcb_button_press_event_t *)ev, false);
                break;
            case XCB_MOTION_NOTIFY:
                handle_motion_event((xcb_motion_notify_event_t *)ev);
                break;
            case XCB_CONFIGURE_NOTIFY:
                handle_configure_event((xcb_configure_notify_event_t *)ev);
                break;
            case XCB_FOCUS_IN:
                // Re-sync xkb state from X server to fix modifier
                // desync (e.g. Ctrl/Shift released while unfocused)
                if (m_xkb_state) xkb_state_unref(m_xkb_state);
                m_xkb_state = xkb_x11_state_new_from_device(m_xkb_keymap, m_conn, m_xkb_device_id);
                if (m_xkb_compose_state) xkb_compose_state_reset(m_xkb_compose_state);
                if (on_focus) on_focus(true);
                break;
            case XCB_FOCUS_OUT:
                if (on_focus) on_focus(false);
                break;
            case XCB_CLIENT_MESSAGE: {
                auto *cm = (xcb_client_message_event_t *)ev;
                if (cm->data.data32[0] == m_atom_wm_delete) {
                    if (on_close) on_close();
                }
                break;
            }
            case XCB_SELECTION_REQUEST:
                handle_selection_request((xcb_selection_request_event_t *)ev);
                break;
            case XCB_SELECTION_NOTIFY:
                handle_selection_notify((xcb_selection_notify_event_t *)ev);
                break;
            case XCB_SELECTION_CLEAR:
                // Another client took the selection; drop our copy so a
                // later paste asks the new owner instead of echoing stale text.
                if (((xcb_selection_clear_event_t *)ev)->selection == m_atom_clipboard) {
                    m_clipboard_text.clear();
                    m_clipboard_typed = {};
                } else {
                    m_primary_text.clear();
                    m_primary_typed = {};
                }
                break;
            case XCB_PROPERTY_NOTIFY:
                handle_property_notify((xcb_property_notify_event_t *)ev);
                break;
            default:
                if (type == m_xkb_first_event) {
                    auto *xkb_ev = (xcb_xkb_state_notify_event_t *)ev;
                    switch (xkb_ev->xkbType) {
                        case XCB_XKB_NEW_KEYBOARD_NOTIFY:
                        case XCB_XKB_MAP_NOTIFY:
                            // Keymap changed — reload from server
                            dbg("xkb: keymap changed, reloading");
                            m_xkb_device_id = xkb_x11_get_core_keyboard_device_id(m_conn);
                            if (m_xkb_state) xkb_state_unref(m_xkb_state);
                            if (m_xkb_keymap) xkb_keymap_unref(m_xkb_keymap);
                            m_xkb_keymap = xkb_x11_keymap_new_from_device(
                                m_xkb_ctx, m_conn, m_xkb_device_id,
                                XKB_KEYMAP_COMPILE_NO_FLAGS);
                            m_xkb_state = xkb_x11_state_new_from_device(
                                m_xkb_keymap, m_conn, m_xkb_device_id);
                            if (m_xkb_compose_state) xkb_compose_state_reset(m_xkb_compose_state);
                            if (m_key_symbols) xcb_key_symbols_free(m_key_symbols);
                            m_key_symbols = xcb_key_symbols_alloc(m_conn);
                            break;
                        case XCB_XKB_STATE_NOTIFY:
                            // Modifier/group state changed externally
                            xkb_state_update_mask(m_xkb_state,
                                xkb_ev->baseMods, xkb_ev->latchedMods, xkb_ev->lockedMods,
                                xkb_ev->baseGroup, xkb_ev->latchedGroup, xkb_ev->lockedGroup);
                            break;
                    }
                }
                break;
        }
        free(ev);
    }

    expire_pastes();
}

void X11Backend::set_clipboard(const std::string &text, bool primary) {
    if (primary) {
        m_primary_text = text;
        xcb_set_selection_owner(m_conn, m_window, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    } else {
        m_clipboard_text = text;
        xcb_set_selection_owner(m_conn, m_window, m_atom_clipboard, XCB_CURRENT_TIME);
    }
    xcb_flush(m_conn);
}

void X11Backend::set_clipboard_data(const std::string &data, const std::string &mime_type, bool primary) {
    ClipboardEntry &entry = primary ? m_primary_typed : m_clipboard_typed;
    entry.data = data;
    entry.mime_type = mime_type;

    xcb_atom_t selection = primary ? (xcb_atom_t)XCB_ATOM_PRIMARY : m_atom_clipboard;
    xcb_set_selection_owner(m_conn, m_window, selection, XCB_CURRENT_TIME);
    xcb_flush(m_conn);
}

// Synchronous reads only work for selections we own. Waiting inline for
// another client's reply would stall the whole event loop — including the
// SelectionRequest handler that other clients are themselves waiting on —
// so everything else goes through request_clipboard().
std::string X11Backend::get_clipboard(bool primary) {
    if (!owns_selection(primary ? (xcb_atom_t)XCB_ATOM_PRIMARY : m_atom_clipboard)) return {};
    return primary ? m_primary_text : m_clipboard_text;
}

std::string X11Backend::get_clipboard_data(const std::string &mime_type, bool primary) {
    if (mime_type.empty() || mime_type == "text/plain") return get_clipboard(primary);
    if (!owns_selection(primary ? (xcb_atom_t)XCB_ATOM_PRIMARY : m_atom_clipboard)) return {};
    const ClipboardEntry &entry = primary ? m_primary_typed : m_clipboard_typed;
    return (entry.mime_type == mime_type) ? entry.data : std::string{};
}

void X11Backend::request_clipboard(bool primary, ClipboardCallback cb) {
    xcb_atom_t selection = primary ? (xcb_atom_t)XCB_ATOM_PRIMARY : m_atom_clipboard;
    if (owns_selection(selection)) {
        cb(primary ? m_primary_text : m_clipboard_text);
        return;
    }
    queue_paste(selection, m_atom_utf8_string, std::move(cb));
}

void X11Backend::request_clipboard_data(const std::string &mime_type, bool primary,
                                        ClipboardCallback cb) {
    if (mime_type.empty() || mime_type == "text/plain") {
        request_clipboard(primary, std::move(cb));
        return;
    }
    if (mime_type != "image/png") {  // only image/png supported for now
        cb({});
        return;
    }
    xcb_atom_t selection = primary ? (xcb_atom_t)XCB_ATOM_PRIMARY : m_atom_clipboard;
    if (owns_selection(selection)) {
        const ClipboardEntry &entry = primary ? m_primary_typed : m_clipboard_typed;
        cb((entry.mime_type == mime_type) ? entry.data : std::string{});
        return;
    }
    queue_paste(selection, m_atom_image_png, std::move(cb));
}

bool X11Backend::owns_selection(xcb_atom_t selection) {
    xcb_get_selection_owner_reply_t *owner = xcb_get_selection_owner_reply(
        m_conn, xcb_get_selection_owner(m_conn, selection), nullptr);
    bool ours = owner && owner->owner == m_window;
    free(owner);
    return ours;
}

void X11Backend::queue_paste(xcb_atom_t selection, xcb_atom_t target, ClipboardCallback cb) {
    PendingPaste p;
    p.selection = selection;
    p.target = target;
    p.cb = std::move(cb);
    m_pastes.push_back(std::move(p));
    if (m_pastes.size() == 1) start_front_paste();
}

void X11Backend::start_front_paste() {
    PendingPaste &p = m_pastes.front();
    p.started = true;
    p.deadline = std::chrono::steady_clock::now() + kPasteTimeout;
    // Drop leftovers so a stale value can't be mistaken for this reply.
    xcb_delete_property(m_conn, m_window, m_atom_rivt_sel);
    xcb_convert_selection(m_conn, m_window, p.selection, p.target,
                          m_atom_rivt_sel, XCB_CURRENT_TIME);
    xcb_flush(m_conn);
}

void X11Backend::finish_front_paste(std::string data) {
    ClipboardCallback cb = std::move(m_pastes.front().cb);
    m_pastes.pop_front();
    // Requests share one property, so the next one can only start now.
    if (!m_pastes.empty() && !m_pastes.front().started) start_front_paste();
    // Last: cb writes to a PTY and may queue another paste.
    if (cb) cb(data);
}

void X11Backend::expire_pastes() {
    if (m_pastes.empty()) return;
    if (std::chrono::steady_clock::now() < m_pastes.front().deadline) return;
    dbg("clipboard: paste request timed out");
    finish_front_paste({});
}

bool X11Backend::read_paste_property(std::string &out, xcb_atom_t &type) {
    out.clear();
    type = XCB_ATOM_NONE;
    uint32_t offset = 0;  // in 32-bit units, as GetProperty wants
    for (;;) {
        xcb_get_property_reply_t *prop = xcb_get_property_reply(
            m_conn, xcb_get_property(m_conn, 0, m_window, m_atom_rivt_sel,
                                     XCB_ATOM_ANY, offset, 64 * 1024), nullptr);
        if (!prop) return false;
        type = prop->type;
        int len = xcb_get_property_value_length(prop);
        uint32_t bytes_after = prop->bytes_after;
        if (out.size() + (size_t)len > kMaxPasteBytes) {
            free(prop);
            fprintf(stderr, "rivt: clipboard paste exceeds %zu bytes, truncating\n",
                    kMaxPasteBytes);
            return true;
        }
        out.append((const char *)xcb_get_property_value(prop), len);
        free(prop);
        if (bytes_after == 0 || len == 0) return true;
        offset += (uint32_t)len / 4;
    }
}

// Serve a selection we own to another client.
void X11Backend::handle_selection_request(xcb_selection_request_event_t *sr) {
    const std::string &text = (sr->selection == XCB_ATOM_PRIMARY) ? m_primary_text : m_clipboard_text;
    const ClipboardEntry &typed = (sr->selection == XCB_ATOM_PRIMARY) ? m_primary_typed : m_clipboard_typed;

    xcb_selection_notify_event_t notify{};
    notify.response_type = XCB_SELECTION_NOTIFY;
    notify.requestor = sr->requestor;
    notify.selection = sr->selection;
    notify.target = sr->target;
    notify.time = sr->time;

    if (sr->target == m_atom_image_png && typed.mime_type == "image/png") {
        if (typed.data.size() <= 256 * 1024) {
            xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE,
                                sr->requestor, sr->property,
                                m_atom_image_png, 8,
                                typed.data.size(), typed.data.data());
            notify.property = sr->property;
        } else {
            // INCR send not implemented yet — reject oversized transfers
            fprintf(stderr, "rivt: clipboard image too large for non-INCR transfer (%zu bytes)\n", typed.data.size());
            notify.property = XCB_ATOM_NONE;
        }
    } else if (sr->target == m_atom_utf8_string || sr->target == XCB_ATOM_STRING) {
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE,
                            sr->requestor, sr->property,
                            m_atom_utf8_string, 8,
                            text.size(), text.c_str());
        notify.property = sr->property;
    } else if (sr->target == m_atom_targets) {
        std::vector<xcb_atom_t> targets = { m_atom_targets, m_atom_utf8_string, XCB_ATOM_STRING };
        if (!typed.data.empty() && typed.mime_type == "image/png")
            targets.push_back(m_atom_image_png);
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE,
                            sr->requestor, sr->property,
                            XCB_ATOM_ATOM, 32, targets.size(), targets.data());
        notify.property = sr->property;
    } else {
        notify.property = XCB_ATOM_NONE;
    }

    // xcb_send_event always copies 32 bytes; the struct is 24.
    char ev_buf[32] = {};
    memcpy(ev_buf, &notify, sizeof(notify));
    xcb_send_event(m_conn, false, sr->requestor, 0, ev_buf);
    xcb_flush(m_conn);
}

void X11Backend::handle_selection_notify(xcb_selection_notify_event_t *sn) {
    // Unsolicited or stale reply (e.g. from a request we already timed out).
    if (m_pastes.empty() || m_pastes.front().selection != sn->selection) return;
    if (sn->property == XCB_ATOM_NONE) {  // owner can't supply this target
        finish_front_paste({});
        return;
    }

    std::string data;
    xcb_atom_t type = XCB_ATOM_NONE;
    if (!read_paste_property(data, type)) {
        finish_front_paste({});
        return;
    }

    if (type == m_atom_incr) {
        // Value is too big for one property: the owner streams it as a
        // series of PropertyNotify chunks, each acknowledged by deleting
        // the property. The value we just read is only a size hint.
        PendingPaste &p = m_pastes.front();
        p.incr = true;
        p.data.clear();
        p.deadline = std::chrono::steady_clock::now() + kPasteTimeout;
        xcb_delete_property(m_conn, m_window, m_atom_rivt_sel);
        xcb_flush(m_conn);
        return;
    }

    xcb_delete_property(m_conn, m_window, m_atom_rivt_sel);
    xcb_flush(m_conn);
    finish_front_paste(std::move(data));
}

void X11Backend::handle_property_notify(xcb_property_notify_event_t *pn) {
    if (pn->window != m_window || pn->atom != m_atom_rivt_sel) return;
    if (pn->state != XCB_PROPERTY_NEW_VALUE) return;
    if (m_pastes.empty() || !m_pastes.front().incr) return;

    std::string chunk;
    xcb_atom_t type = XCB_ATOM_NONE;
    if (!read_paste_property(chunk, type)) {
        finish_front_paste({});
        return;
    }
    xcb_delete_property(m_conn, m_window, m_atom_rivt_sel);  // ask for the next chunk
    xcb_flush(m_conn);

    if (chunk.empty()) {  // zero-length property terminates the transfer
        finish_front_paste(std::move(m_pastes.front().data));
        return;
    }

    PendingPaste &p = m_pastes.front();
    p.data += chunk;
    p.deadline = std::chrono::steady_clock::now() + kPasteTimeout;
}

float X11Backend::get_dpi_scale() {
    // Try to get DPI from X resources
    // For now, return 1.0 as default
    // TODO: query Xrdb for Xft.dpi or use xrandr
    return 1.0f;
}

void X11Backend::create_cursors() {
    // Open the standard cursor font
    xcb_font_t font = xcb_generate_id(m_conn);
    xcb_open_font(m_conn, font, 6, "cursor");

    // XC_left_ptr = 68, XC_hand2 = 60, XC_xterm = 152
    m_cursor_default = xcb_generate_id(m_conn);
    xcb_create_glyph_cursor(m_conn, m_cursor_default, font, font, 68, 69, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0);

    m_cursor_hand = xcb_generate_id(m_conn);
    xcb_create_glyph_cursor(m_conn, m_cursor_hand, font, font, 60, 61, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0);

    m_cursor_text = xcb_generate_id(m_conn);
    xcb_create_glyph_cursor(m_conn, m_cursor_text, font, font, 152, 153, 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0);

    xcb_close_font(m_conn, font);
}

void X11Backend::set_mouse_cursor(MouseCursor mc) {
    if (mc == m_current_cursor) return;
    m_current_cursor = mc;

    xcb_cursor_t cursor = m_cursor_default;
    if (mc == MouseCursor::Hand) cursor = m_cursor_hand;
    else if (mc == MouseCursor::Text) cursor = m_cursor_text;

    if (cursor) {
        uint32_t values[] = { cursor };
        xcb_change_window_attributes(m_conn, m_window, XCB_CW_CURSOR, values);
        xcb_flush(m_conn);
    }
}

} // namespace rivt
