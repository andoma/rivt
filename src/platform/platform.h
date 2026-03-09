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

    // GL context
    virtual bool create_gl_context() = 0;
    virtual void make_current() = 0;
    virtual void swap_buffers() = 0;

    // Event fd for epoll integration
    virtual int get_event_fd() = 0;

    // Process pending events, call registered callbacks
    virtual void process_events() = 0;

    // Clipboard
    virtual void set_clipboard(const std::string &text, bool primary = false) = 0;
    virtual std::string get_clipboard(bool primary = false) = 0;

    // Display info
    virtual float get_dpi_scale() = 0;

    // Callbacks
    std::function<void(const KeyEvent &)> on_key;
    std::function<void(const MouseEvent &)> on_mouse;
    std::function<void(int width, int height)> on_resize;
    std::function<void(bool focused)> on_focus;
    std::function<void()> on_close;

    static std::unique_ptr<Platform> create();
};

} // namespace rivt
