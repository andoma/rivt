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

    Platform *platform() { return m_platform.get(); }
    int event_fd() const { return m_platform->get_event_fd(); }
    bool is_closing() const { return m_closing; }
    void mark_closing() { m_closing = true; }
    bool needs_render() const { return m_needs_render; }
    TabManager *tab_manager() { return m_tabs.get(); }

    // Resize window to fit a given cell grid (used by tmux controller)
    void resize_to_cells(int cols, int rows);

    // Grow/shrink the X11 window when the tab bar appears/disappears,
    // keeping the terminal content area unchanged.
    void adjust_tab_bar_height();

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

    Config m_config;
    EventLoop &m_loop;
    std::unique_ptr<Platform> m_platform;
    Renderer m_renderer;
    std::unique_ptr<TabManager> m_tabs;
    int m_win_w = 800, m_win_h = 600;
    int m_last_bar_h = 0;  // tracks tab bar height for grow/shrink
    bool m_needs_render = true;
    bool m_focused = true;
    bool m_cursor_blink_on = true;
    bool m_closing = false;

    std::unique_ptr<TmuxClient> m_tmux_client;
    std::unique_ptr<TmuxController> m_tmux_controller;
    Pane *m_tmux_gateway_pane = nullptr;  // pane whose PTY carries tmux traffic

    // Deferred destruction — can't destroy while inside feed_data() call stack
    std::unique_ptr<TmuxClient> m_tmux_stale_client;
    std::unique_ptr<TmuxController> m_tmux_stale_controller;
};

} // namespace rivt
