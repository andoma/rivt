#pragma once
#include "core/config.h"
#include "core/tab_manager.h"
#include "core/input_encoder.h"
#include "render/renderer.h"
#include "platform/platform.h"
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace rivt {

class EventLoop;
class TmuxClient;
class TmuxController;

class Window {
public:
    Window(const Config &base_config, EventLoop &loop);
    ~Window();

    bool init();
    bool init_tmux(const std::vector<std::string> &tmux_args);
    bool init_tmux_pty(Pane *gateway_pane);
    void render_if_needed();
    bool reap_dead_panes();
    void toggle_cursor_blink();

    Platform *platform() { return platform_.get(); }
    int event_fd() const { return platform_->get_event_fd(); }
    bool is_closing() const { return closing_; }
    void mark_closing() { closing_ = true; }
    bool needs_render() const { return needs_render_; }
    TabManager *tab_manager() { return tabs_.get(); }

    // Resize window to fit a given cell grid (used by tmux controller)
    void resize_to_cells(int cols, int rows);

    std::function<void()> on_new_window;
    std::function<void(Pane *gateway)> on_new_tmux_window;
    std::function<void(Window *)> on_close;

    int tab_bar_height() const;

private:
    void setup_callbacks();
    void recompute();
    void update_size_hints();
    void resize_font();
    void handle_key(const KeyEvent &key);
    void handle_mouse(const MouseEvent &mouse);
    void handle_resize(int w, int h);
    void start_tmux_from_pane(Pane *gateway);
    void stop_tmux_pty_mode();

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

    std::unique_ptr<TmuxClient> tmux_client_;
    std::unique_ptr<TmuxController> tmux_controller_;
    Pane *tmux_gateway_pane_ = nullptr;  // pane whose PTY carries tmux traffic

    // Deferred destruction — can't destroy while inside feed_data() call stack
    std::unique_ptr<TmuxClient> tmux_stale_client_;
    std::unique_ptr<TmuxController> tmux_stale_controller_;
};

} // namespace rivt
