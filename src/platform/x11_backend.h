#pragma once
#include "platform/platform.h"
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <EGL/egl.h>
#include <X11/Xlib.h>
#include <chrono>
#include <deque>

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
    void request_clipboard(bool primary, ClipboardCallback cb) override;
    void request_clipboard_data(const std::string &mime_type, bool primary,
                                ClipboardCallback cb) override;

    float get_dpi_scale() override;
    void set_mouse_cursor(MouseCursor cursor) override;
    bool is_obscured() const override { return m_obscured; }

private:
    void handle_key_event(xcb_key_press_event_t *ev, bool pressed);
    void handle_button_event(xcb_button_press_event_t *ev, bool pressed);
    void handle_motion_event(xcb_motion_notify_event_t *ev);
    void handle_configure_event(xcb_configure_notify_event_t *ev);
    void handle_selection_request(xcb_selection_request_event_t *ev);
    void handle_selection_notify(xcb_selection_notify_event_t *ev);
    void handle_property_notify(xcb_property_notify_event_t *ev);
    KeyMod translate_mods(uint16_t state);

    xcb_connection_t *m_conn = nullptr;
    xcb_screen_t *m_screen = nullptr;
    xcb_window_t m_window = 0;
    xcb_key_symbols_t *m_key_symbols = nullptr;

    struct xkb_context *m_xkb_ctx = nullptr;
    struct xkb_keymap *m_xkb_keymap = nullptr;
    struct xkb_state *m_xkb_state = nullptr;
    struct xkb_compose_table *m_xkb_compose_table = nullptr;
    struct xkb_compose_state *m_xkb_compose_state = nullptr;
    int32_t m_xkb_device_id = -1;
    uint8_t m_xkb_first_event = 0;

    EGLDisplay m_egl_display = EGL_NO_DISPLAY;
    EGLContext m_egl_context = EGL_NO_CONTEXT;
    EGLSurface m_egl_surface = EGL_NO_SURFACE;

    Display *m_xlib_display = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_obscured = false;

    // Clipboard
    xcb_atom_t m_atom_clipboard = 0;
    xcb_atom_t m_atom_utf8_string = 0;
    xcb_atom_t m_atom_targets = 0;
    xcb_atom_t m_atom_rivt_sel = 0;
    xcb_atom_t m_atom_wm_protocols = 0;
    xcb_atom_t m_atom_wm_delete = 0;
    xcb_atom_t m_atom_image_png = 0;
    xcb_atom_t m_atom_incr = 0;
    std::string m_clipboard_text;
    std::string m_primary_text;

    // Reads from a selection we don't own are asynchronous: the owner
    // replies with a SelectionNotify, and large transfers arrive as a
    // stream of PropertyNotify chunks (INCR). Requests are parked here
    // until they complete or time out. Only the front one is in flight,
    // since they all share the m_atom_rivt_sel property.
    struct PendingPaste {
        xcb_atom_t selection = 0;
        xcb_atom_t target = 0;
        ClipboardCallback cb;
        std::string data;   // accumulated INCR chunks
        bool incr = false;  // INCR transfer in progress
        bool started = false;
        std::chrono::steady_clock::time_point deadline;
    };
    std::deque<PendingPaste> m_pastes;

    bool owns_selection(xcb_atom_t selection);
    void queue_paste(xcb_atom_t selection, xcb_atom_t target, ClipboardCallback cb);
    void start_front_paste();
    void finish_front_paste(std::string data);
    void expire_pastes();
    // Reads m_atom_rivt_sel in full without deleting it. Returns false if
    // the property is gone.
    bool read_paste_property(std::string &out, xcb_atom_t &type);

    // Typed clipboard storage
    struct ClipboardEntry {
        std::string data;
        std::string mime_type;
    };
    ClipboardEntry m_clipboard_typed;
    ClipboardEntry m_primary_typed;

    xcb_atom_t intern_atom(const char *name);
    void create_cursors();

    xcb_cursor_t m_cursor_default = 0;
    xcb_cursor_t m_cursor_hand = 0;
    xcb_cursor_t m_cursor_text = 0;
    xcb_cursor_t m_cursor_resize_h = 0;
    xcb_cursor_t m_cursor_resize_v = 0;
    MouseCursor m_current_cursor = MouseCursor::Default;
};

} // namespace rivt
