#pragma once
#include "tmux/tmux_client.h"
#include "core/event_loop.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace rivt {

class Window;
class TabManager;
class Tab;
class Pane;

class TmuxController {
public:
    TmuxController(TmuxClient &client, Window &window, TabManager &tabs, EventLoop &loop);

    void initialize(int cols, int rows, int cell_w, int cell_h, int content_x, int content_y);
    void handle_resize(int cols, int rows, int cell_w, int cell_h, int content_x, int content_y);
    void detach();
    bool is_active() const { return active_; }

    // Set the gateway pane (the pane whose PTY carries tmux traffic in PTY mode)
    void set_gateway_pane(Pane *pane) { gateway_pane_ = pane; }

    // Callback: fired when tmux exits in PTY mode (so Window can clean up)
    std::function<void()> on_tmux_exit;

private:
    void on_output(int pane_id, const std::string &data);
    void on_window_add(int window_id);
    void on_window_close(int window_id);
    void on_window_renamed(int window_id, const std::string &name);
    void on_layout_change(int window_id, const std::string &layout_str, bool is_active = false);
    void on_session_changed();
    void on_session_window_changed(int window_id);
    void on_exit();

    Pane *create_tmux_pane(Tab *tab, int tmux_pane_id, int cols, int rows);

public:
    struct PaneGeom {
        int pane_id, x, y, w, h;
    };
    static std::vector<PaneGeom> parse_layout(const std::string &layout);

private:
    TmuxClient &client_;
    Window &window_;
    TabManager &tabs_;
    EventLoop &loop_;
    bool active_ = false;

    std::unordered_map<int, Pane *> pane_map_;    // tmux %pane_id -> rivt Pane*
    std::unordered_map<int, Tab *> window_map_;   // tmux @window_id -> rivt Tab*

    std::unordered_map<int, std::string> output_buffer_;

    int cell_w_ = 0, cell_h_ = 0;
    int content_x_ = 0, content_y_ = 0;
    bool initial_resize_done_ = false;

    Pane *gateway_pane_ = nullptr;  // PTY mode only
};

} // namespace rivt
