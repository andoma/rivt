#pragma once
#include "core/types.h"
#include <functional>
#include <string>
#include <memory>

namespace rivt {

class Platform {
public:
    virtual ~Platform() = default;

    // Window management
    virtual bool create_window(int width, int height, const std::string &title) = 0;
    virtual void destroy_window() = 0;
    virtual void set_title(const std::string &title) = 0;
    virtual void get_size(int &width, int &height) = 0;
    virtual void resize_window(int width, int height) = 0;
    virtual void show_window() = 0;
    virtual void set_size_hints(int cell_w, int cell_h, int base_w, int base_h) = 0;

    // GL context
    virtual bool create_gl_context() = 0;
    virtual void make_current() = 0;
    virtual void swap_buffers() = 0;

    // Event fd selectable by EventLoop. -1 indicates the platform has no
    // per-window fd (e.g. Cocoa, where NSApp dispatches events centrally).
    virtual int get_event_fd() = 0;

    // Process pending events, call registered callbacks
    virtual void process_events() = 0;

    // Clipboard
    virtual void set_clipboard(const std::string &text, bool primary = false) = 0;
    virtual std::string get_clipboard(bool primary = false) = 0;

    // Typed clipboard (for Kitty clipboard protocol)
    virtual void set_clipboard_data(const std::string &data, const std::string &mime_type, bool primary = false) {
        // Default: fall back to text clipboard for text types
        if (mime_type.empty() || mime_type == "text/plain")
            set_clipboard(data, primary);
    }
    virtual std::string get_clipboard_data(const std::string &mime_type, bool primary = false) {
        if (mime_type.empty() || mime_type == "text/plain")
            return get_clipboard(primary);
        return {};
    }

    // Mouse cursor appearance
    enum class MouseCursor { Default, Hand, Text };
    virtual void set_mouse_cursor(MouseCursor /*cursor*/) {}

    // Display info
    virtual float get_dpi_scale() = 0;

    // Callbacks
    std::function<void(const KeyEvent &)> on_key;
    std::function<void(const MouseEvent &)> on_mouse;
    std::function<void(int width, int height)> on_resize;
    std::function<void(bool focused)> on_focus;
    std::function<void()> on_close;

    // Menu actions (macOS native menu bar). Fired when the user picks
    // File > New Window / Close Window or Edit > Copy / Paste. No-op on
    // platforms without a native menu bar; rivt's existing keyboard
    // shortcuts (Ctrl-Shift-C/V etc.) still work.
    std::function<void()> on_menu_new_window;
    std::function<void()> on_menu_close_window;
    std::function<void()> on_menu_copy;
    std::function<void()> on_menu_paste;

    static std::unique_ptr<Platform> create();
};

} // namespace rivt
