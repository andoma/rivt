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
    bool is_active() const { return m_active; }

    // Set the gateway pane (the pane whose PTY carries tmux traffic in PTY mode)
    void set_gateway_pane(Pane *pane) { m_gateway_pane = pane; }

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
    void request_window_names();
    void reposition_all_panes();
    void on_exit();

    Pane *create_tmux_pane(Tab *tab, int tmux_pane_id, int cols, int rows);

public:
    struct PaneGeom {
        int pane_id, x, y, w, h;
    };
    static std::vector<PaneGeom> parse_layout(const std::string &layout);

private:
    TmuxClient &m_client;
    Window &m_window;
    TabManager &m_tabs;
    EventLoop &m_loop;
    bool m_active = false;

    std::unordered_map<int, Pane *> m_pane_map;    // tmux %pane_id -> rivt Pane*
    std::unordered_map<int, Tab *> m_window_map;   // tmux @window_id -> rivt Tab*

    std::unordered_map<int, std::string> m_output_buffer;

    int m_cell_w = 0, m_cell_h = 0;
    int m_content_x = 0, m_content_y = 0;
    bool m_initial_resize_done = false;

    Pane *m_gateway_pane = nullptr;  // PTY mode only
};

} // namespace rivt
