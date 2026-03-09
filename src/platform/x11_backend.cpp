#include "platform/x11_backend.h"
#include <xcb/xcb_icccm.h>
#include <cstring>
#include <cstdlib>
#include <stdexcept>

// EGL needs the X11 display for its native display type
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

namespace rivt {

X11Backend::X11Backend() {
    xkb_ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx_)
        throw std::runtime_error("Failed to create xkb context");
}

X11Backend::~X11Backend() {
    destroy_window();
    if (xkb_state_) xkb_state_unref(xkb_state_);
    if (xkb_keymap_) xkb_keymap_unref(xkb_keymap_);
    if (xkb_ctx_) xkb_context_unref(xkb_ctx_);
}

xcb_atom_t X11Backend::intern_atom(const char *name) {
    xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn_, 0, strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn_, cookie, nullptr);
    xcb_atom_t atom = reply ? reply->atom : (xcb_atom_t)XCB_ATOM_NONE;
    free(reply);
    return atom;
}

bool X11Backend::create_window(int width, int height, const std::string &title) {
    // Use Xlib to get both Display* (for EGL) and xcb connection
    xlib_display_ = XOpenDisplay(nullptr);
    if (!xlib_display_) return false;

    conn_ = XGetXCBConnection(xlib_display_);
    if (!conn_ || xcb_connection_has_error(conn_)) return false;

    // Let XCB own the event queue
    XSetEventQueueOwner(xlib_display_, XCBOwnsEventQueue);

    screen_ = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data;

    window_ = xcb_generate_id(conn_);
    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] = {
        screen_->black_pixel,
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_FOCUS_CHANGE |
        XCB_EVENT_MASK_PROPERTY_CHANGE
    };

    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, screen_->root,
                      0, 0, width, height, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen_->root_visual, mask, values);

    width_ = width;
    height_ = height;

    // Intern atoms
    atom_clipboard_ = intern_atom("CLIPBOARD");
    atom_utf8_string_ = intern_atom("UTF8_STRING");
    atom_targets_ = intern_atom("TARGETS");
    atom_rivt_sel_ = intern_atom("RIVT_SELECTION");
    atom_wm_protocols_ = intern_atom("WM_PROTOCOLS");
    atom_wm_delete_ = intern_atom("WM_DELETE_WINDOW");

    // Register for WM_DELETE_WINDOW
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
                        atom_wm_protocols_, XCB_ATOM_ATOM, 32, 1, &atom_wm_delete_);

    set_title(title);
    xcb_map_window(conn_, window_);
    xcb_flush(conn_);

    // Setup xkbcommon-x11
    xkb_x11_setup_xkb_extension(conn_,
        XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
        XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, nullptr, nullptr, nullptr, nullptr);

    xkb_device_id_ = xkb_x11_get_core_keyboard_device_id(conn_);
    xkb_keymap_ = xkb_x11_keymap_new_from_device(xkb_ctx_, conn_, xkb_device_id_,
                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_state_ = xkb_x11_state_new_from_device(xkb_keymap_, conn_, xkb_device_id_);

    key_symbols_ = xcb_key_symbols_alloc(conn_);

    // EGL setup
    egl_display_ = eglGetDisplay((EGLNativeDisplayType)xlib_display_);
    if (egl_display_ == EGL_NO_DISPLAY) return false;

    EGLint major, minor;
    if (!eglInitialize(egl_display_, &major, &minor)) return false;

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
    if (!eglChooseConfig(egl_display_, config_attribs, &config, 1, &num_configs) || num_configs == 0)
        return false;

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };

    egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context_ == EGL_NO_CONTEXT) return false;

    egl_surface_ = eglCreateWindowSurface(egl_display_, config,
                                           (EGLNativeWindowType)window_, nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) return false;

    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
    return true;
}

void X11Backend::make_current() {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
}

void X11Backend::swap_buffers() {
    eglSwapBuffers(egl_display_, egl_surface_);
}

void X11Backend::destroy_window() {
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (egl_surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display_, egl_context_);
        egl_context_ = EGL_NO_CONTEXT;
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    eglReleaseThread();
    if (key_symbols_) {
        xcb_key_symbols_free(key_symbols_);
        key_symbols_ = nullptr;
    }
    if (window_ && conn_) {
        xcb_destroy_window(conn_, window_);
        window_ = 0;
    }
    // XCloseDisplay closes the underlying xcb connection too
    if (xlib_display_) {
        XCloseDisplay(xlib_display_);
        xlib_display_ = nullptr;
        conn_ = nullptr;
    }
}

void X11Backend::set_title(const std::string &title) {
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
                        XCB_ATOM_WM_NAME, atom_utf8_string_, 8,
                        title.size(), title.c_str());
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_,
                        intern_atom("_NET_WM_NAME"), atom_utf8_string_, 8,
                        title.size(), title.c_str());
    xcb_flush(conn_);
}

void X11Backend::get_size(int &width, int &height) {
    width = width_;
    height = height_;
}

int X11Backend::get_event_fd() {
    return xcb_get_file_descriptor(conn_);
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
    xkb_state_update_key(xkb_state_, ev->detail, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

    KeyEvent key{};
    key.pressed = pressed;
    key.mods = translate_mods(ev->state);

    // Get the keysym with Shift applied but without Ctrl, so we see 'c'/'C' not a
    // control character. This lets Ctrl+C map to ETX and Ctrl+Shift+C match XKB_KEY_C.
    {
        const xkb_keysym_t *syms;
        xkb_layout_index_t layout = xkb_state_key_get_layout(xkb_state_, ev->detail);
        // level 0 = unshifted, level 1 = shifted
        int level = (ev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0;
        int n = xkb_keymap_key_get_syms_by_level(xkb_keymap_, ev->detail, layout, level, &syms);
        key.keysym = (n > 0) ? syms[0] : xkb_state_key_get_one_sym(xkb_state_, ev->detail);
    }

    if (pressed) {
        char buf[64];
        int len = xkb_state_key_get_utf8(xkb_state_, ev->detail, buf, sizeof(buf));
        if (len > 0 && buf[0] >= 0x20) {
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
    if (ev->width != width_ || ev->height != height_) {
        width_ = ev->width;
        height_ = ev->height;
        if (on_resize) on_resize(width_, height_);
    }
}

void X11Backend::process_events() {
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(conn_)) != nullptr) {
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
                if (on_focus) on_focus(true);
                break;
            case XCB_FOCUS_OUT:
                if (on_focus) on_focus(false);
                break;
            case XCB_CLIENT_MESSAGE: {
                auto *cm = (xcb_client_message_event_t *)ev;
                if (cm->data.data32[0] == atom_wm_delete_) {
                    if (on_close) on_close();
                }
                break;
            }
            case XCB_SELECTION_REQUEST: {
                // Respond to selection requests from other apps
                auto *sr = (xcb_selection_request_event_t *)ev;
                const std::string &text = (sr->selection == XCB_ATOM_PRIMARY) ? primary_text_ : clipboard_text_;

                xcb_selection_notify_event_t notify{};
                notify.response_type = XCB_SELECTION_NOTIFY;
                notify.requestor = sr->requestor;
                notify.selection = sr->selection;
                notify.target = sr->target;
                notify.time = sr->time;

                if (sr->target == atom_utf8_string_ || sr->target == XCB_ATOM_STRING) {
                    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE,
                                        sr->requestor, sr->property,
                                        atom_utf8_string_, 8,
                                        text.size(), text.c_str());
                    notify.property = sr->property;
                } else if (sr->target == atom_targets_) {
                    xcb_atom_t targets[] = { atom_targets_, atom_utf8_string_, XCB_ATOM_STRING };
                    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE,
                                        sr->requestor, sr->property,
                                        XCB_ATOM_ATOM, 32, 3, targets);
                    notify.property = sr->property;
                } else {
                    notify.property = XCB_ATOM_NONE;
                }

                xcb_send_event(conn_, false, sr->requestor, 0, (const char *)&notify);
                xcb_flush(conn_);
                break;
            }
            default:
                break;
        }
        free(ev);
    }
}

void X11Backend::set_clipboard(const std::string &text, bool primary) {
    if (primary) {
        primary_text_ = text;
        xcb_set_selection_owner(conn_, window_, XCB_ATOM_PRIMARY, XCB_CURRENT_TIME);
    } else {
        clipboard_text_ = text;
        xcb_set_selection_owner(conn_, window_, atom_clipboard_, XCB_CURRENT_TIME);
    }
    xcb_flush(conn_);
}

std::string X11Backend::get_clipboard(bool primary) {
    xcb_atom_t selection = primary ? (xcb_atom_t)XCB_ATOM_PRIMARY : atom_clipboard_;

    // Check if we own it
    xcb_get_selection_owner_cookie_t owner_cookie = xcb_get_selection_owner(conn_, selection);
    xcb_get_selection_owner_reply_t *owner = xcb_get_selection_owner_reply(conn_, owner_cookie, nullptr);
    if (owner && owner->owner == window_) {
        free(owner);
        return primary ? primary_text_ : clipboard_text_;
    }
    free(owner);

    // Request from owner
    xcb_convert_selection(conn_, window_, selection, atom_utf8_string_,
                          atom_rivt_sel_, XCB_CURRENT_TIME);
    xcb_flush(conn_);

    // Wait for SelectionNotify (with timeout)
    for (int i = 0; i < 50; i++) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn_);
        if (!ev) break;
        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_SELECTION_NOTIFY) {
            auto *sn = (xcb_selection_notify_event_t *)ev;
            if (sn->property != XCB_ATOM_NONE) {
                xcb_get_property_cookie_t prop_cookie = xcb_get_property(
                    conn_, 1, window_, atom_rivt_sel_, XCB_ATOM_ANY, 0, 1 << 20);
                xcb_get_property_reply_t *prop = xcb_get_property_reply(conn_, prop_cookie, nullptr);
                std::string result;
                if (prop) {
                    result = std::string((char *)xcb_get_property_value(prop),
                                         xcb_get_property_value_length(prop));
                    free(prop);
                }
                free(ev);
                return result;
            }
            free(ev);
            break;
        }
        free(ev);
    }
    return "";
}

float X11Backend::get_dpi_scale() {
    // Try to get DPI from X resources
    // For now, return 1.0 as default
    // TODO: query Xrdb for Xft.dpi or use xrandr
    return 1.0f;
}

} // namespace rivt
