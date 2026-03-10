#pragma once
#include "platform/platform.h"
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <EGL/egl.h>
#include <X11/Xlib.h>

namespace rivt {

class X11Backend : public Platform {
public:
    X11Backend();
    ~X11Backend() override;

    bool create_window(int width, int height, const std::string &title) override;
    void destroy_window() override;
    void set_title(const std::string &title) override;
    void get_size(int &width, int &height) override;
    void resize_window(int width, int height) override;
    void show_window() override;
    void set_size_hints(int cell_w, int cell_h, int base_w, int base_h) override;

    bool create_gl_context() override;
    void make_current() override;
    void swap_buffers() override;

    int get_event_fd() override;
    void process_events() override;

    void set_clipboard(const std::string &text, bool primary) override;
    std::string get_clipboard(bool primary) override;
    void set_clipboard_data(const std::string &data, const std::string &mime_type, bool primary) override;
    std::string get_clipboard_data(const std::string &mime_type, bool primary) override;

    float get_dpi_scale() override;

private:
    void handle_key_event(xcb_key_press_event_t *ev, bool pressed);
    void handle_button_event(xcb_button_press_event_t *ev, bool pressed);
    void handle_motion_event(xcb_motion_notify_event_t *ev);
    void handle_configure_event(xcb_configure_notify_event_t *ev);
    KeyMod translate_mods(uint16_t state);

    xcb_connection_t *conn_ = nullptr;
    xcb_screen_t *screen_ = nullptr;
    xcb_window_t window_ = 0;
    xcb_key_symbols_t *key_symbols_ = nullptr;

    struct xkb_context *xkb_ctx_ = nullptr;
    struct xkb_keymap *xkb_keymap_ = nullptr;
    struct xkb_state *xkb_state_ = nullptr;
    int32_t xkb_device_id_ = -1;

    EGLDisplay egl_display_ = EGL_NO_DISPLAY;
    EGLContext egl_context_ = EGL_NO_CONTEXT;
    EGLSurface egl_surface_ = EGL_NO_SURFACE;

    Display *xlib_display_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    // Clipboard
    xcb_atom_t atom_clipboard_ = 0;
    xcb_atom_t atom_utf8_string_ = 0;
    xcb_atom_t atom_targets_ = 0;
    xcb_atom_t atom_rivt_sel_ = 0;
    xcb_atom_t atom_wm_protocols_ = 0;
    xcb_atom_t atom_wm_delete_ = 0;
    xcb_atom_t atom_image_png_ = 0;
    std::string clipboard_text_;
    std::string primary_text_;

    // Typed clipboard storage
    struct ClipboardEntry {
        std::string data;
        std::string mime_type;
    };
    ClipboardEntry clipboard_typed_;
    ClipboardEntry primary_typed_;

    xcb_atom_t intern_atom(const char *name);
};

} // namespace rivt
