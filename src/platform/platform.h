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

    // Asynchronous clipboard read. cb receives the contents, or an empty
    // string if the request failed or timed out. On X11 the data comes from
    // another client, so cb can run many event-loop iterations later (and
    // may never run if this Platform is destroyed first). Platforms with a
    // synchronous clipboard invoke it inline.
    using ClipboardCallback = std::function<void(const std::string &)>;
    virtual void request_clipboard(bool primary, ClipboardCallback cb) {
        cb(get_clipboard(primary));
    }
    virtual void request_clipboard_data(const std::string &mime_type, bool primary,
                                        ClipboardCallback cb) {
        cb(get_clipboard_data(mime_type, primary));
    }

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
    enum class MouseCursor { Default, Hand, Text, ResizeH, ResizeV };
    virtual void set_mouse_cursor(MouseCursor /*cursor*/) {}

    // Display info
    virtual float get_dpi_scale() = 0;

    // True while the window is fully covered by other windows. Rendering
    // an obscured window is not just wasted work: on X11 without a
    // compositor the present-complete event never arrives and
    // eglSwapBuffers blocks ~1s per frame, stalling the shared event
    // loop (and every other window) with it.
    virtual bool is_obscured() const { return false; }

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

    // Process-wide handlers used by macOS for menu items and dock-icon
    // interaction (Cmd-N from the menu when no window is focused; Cmd-Q
    // routed through the main loop so destructors run; dock-click reopen).
    // No-op on platforms that don't surface them (Linux/X11 keeps the
    // existing per-window callback model).
    static void set_new_window_handler(std::function<void()> handler);
    static void set_quit_handler(std::function<void()> handler);
    // Connectivity/power events. Sleep is sticky: it parks all remote
    // maintenance until a real Wake — path events during dark-wake
    // windows (lid closed, network "up") must not resume anything.
    // Only the macOS backend emits these today.
    enum class ConnEvent { Sleep, Wake, PathUp, PathDown };
    static void set_connectivity_handler(std::function<void(ConnEvent)> handler);
    static const std::function<void(ConnEvent)> &connectivity_handler();
    static const std::function<void()> &new_window_handler();
    static const std::function<void()> &quit_handler();
};

} // namespace rivt
