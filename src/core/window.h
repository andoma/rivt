#pragma once
#include "core/config.h"
#include "core/tab_manager.h"
#include "core/input_encoder.h"
#include "render/renderer.h"
#include "platform/platform.h"
#include <functional>
#include <memory>

namespace rivt {

class EventLoop;

class Window {
public:
    Window(const Config &base_config, EventLoop &loop);

    bool init();
    void render_if_needed();
    bool reap_dead_panes();
    void toggle_cursor_blink();

    Platform *platform() { return platform_.get(); }
    int event_fd() const { return platform_->get_event_fd(); }
    bool is_closing() const { return closing_; }
    void mark_closing() { closing_ = true; }
    bool needs_render() const { return needs_render_; }

    std::function<void()> on_new_window;
    std::function<void(Window *)> on_close;

private:
    void setup_callbacks();
    void recompute();
    int tab_bar_height() const;
    void resize_font();
    void handle_key(const KeyEvent &key);
    void handle_mouse(const MouseEvent &mouse);
    void handle_resize(int w, int h);

    Config config_;
    EventLoop &loop_;
    std::unique_ptr<Platform> platform_;
    Renderer renderer_;
    std::unique_ptr<TabManager> tabs_;
    int win_w_ = 800, win_h_ = 600;
    bool needs_render_ = true;
    bool focused_ = true;
    bool cursor_blink_on_ = true;
    bool closing_ = false;
};

} // namespace rivt
