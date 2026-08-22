#pragma once
#include "remote/remote_client.h"
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace rivt {

class Window;
class TabManager;
class Tab;
class Pane;

// Maps rivtd sessions onto local tabs/panes — the rivtd sibling of
// TmuxController. v1: one window attaches to one session; sessions are
// single-pane. The remote tab is tmux_managed (pane rects are set here,
// not by the layout engine).
class RemoteController {
public:
    RemoteController(RemoteClient &client, Window &window, TabManager &tabs);

    // Attach to the newest existing session, or create one if none.
    // cols/rows: our window's grid; the daemon pane is resized to it.
    void initialize(int cols, int rows, int cell_w, int cell_h,
                    int content_x, int content_y);
    void handle_resize(int cols, int rows, int cell_w, int cell_h,
                       int content_x, int content_y);

    bool is_active() const { return m_active; }
    void detach();

    // User actions, routed to the daemon.
    void split(bool horizontal);
    void close_focused_pane();
    void new_window();
    // Returns true if the tab is remote-managed and its close was
    // requested from the daemon (teardown happens on WindowClosed).
    bool request_close_tab(Tab *tab);

    // Fired when the session ends or the daemon goes away.
    std::function<void()> on_exit;

private:
    Pane *create_remote_pane(Tab *tab, uint32_t pane_id, int cols, int rows);
    void apply_layout(uint32_t wid, int cols, int rows,
                      const std::vector<RemotePaneGeom> &panes);
    uint32_t focused_pane_id() const;
    void exit();

    RemoteClient &m_client;
    Window &m_window;
    TabManager &m_tabs;

    struct PaneEntry { Pane *pane; uint32_t wid; };

    bool m_active = false;
    uint32_t m_session_id = 0;
    std::unordered_map<uint32_t, Tab *> m_windows;     // wid -> tab
    std::unordered_map<uint32_t, PaneEntry> m_pane_map;

    int m_cols = 80, m_rows = 24;
    int m_cell_w = 0, m_cell_h = 0;
    int m_content_x = 0, m_content_y = 0;
};

} // namespace rivt
