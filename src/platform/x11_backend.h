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

    xcb_connection_t *m_conn = nullptr;
    xcb_screen_t *m_screen = nullptr;
    xcb_window_t m_window = 0;
    xcb_key_symbols_t *m_key_symbols = nullptr;

    struct xkb_context *m_xkb_ctx = nullptr;
    struct xkb_keymap *m_xkb_keymap = nullptr;
    struct xkb_state *m_xkb_state = nullptr;
    int32_t m_xkb_device_id = -1;
    uint8_t m_xkb_first_event = 0;

    EGLDisplay m_egl_display = EGL_NO_DISPLAY;
    EGLContext m_egl_context = EGL_NO_CONTEXT;
    EGLSurface m_egl_surface = EGL_NO_SURFACE;

    Display *m_xlib_display = nullptr;
    int m_width = 0;
    int m_height = 0;

    // Clipboard
    xcb_atom_t m_atom_clipboard = 0;
    xcb_atom_t m_atom_utf8_string = 0;
    xcb_atom_t m_atom_targets = 0;
    xcb_atom_t m_atom_rivt_sel = 0;
    xcb_atom_t m_atom_wm_protocols = 0;
    xcb_atom_t m_atom_wm_delete = 0;
    xcb_atom_t m_atom_image_png = 0;
    std::string m_clipboard_text;
    std::string m_primary_text;

    // Typed clipboard storage
    struct ClipboardEntry {
        std::string data;
        std::string mime_type;
    };
    ClipboardEntry m_clipboard_typed;
    ClipboardEntry m_primary_typed;

    xcb_atom_t intern_atom(const char *name);
};

} // namespace rivt
