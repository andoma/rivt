#include "remote/remote_controller.h"
#include "core/debug.h"
#include "core/pane.h"
#include "core/tab_manager.h"
#include "core/window.h"
#include "proto/snapshot.h"

namespace rivt {

RemoteController::RemoteController(RemoteClient &client, Window &window, TabManager &tabs)
    : m_client(client), m_window(window), m_tabs(tabs) {

    m_client.on_hello_ok = [this]() { m_client.list_sessions(); };

    m_client.on_session_list = [this](const std::vector<RemoteSessionInfo> &list) {
        if (m_active) return;  // roster refresh, not attach flow
        if (list.empty()) {
            dbg("remote: no sessions, creating one (%dx%d)", m_cols, m_rows);
            m_client.create_session("", "", m_cols, m_rows);
        } else {
            uint32_t newest = list.back().id;
            dbg("remote: attaching to session %u", newest);
            m_client.attach(newest);
        }
    };

    m_client.on_session_created = [this](uint32_t sid, uint32_t pane, const std::string &err) {
        if (pane == 0) {
            fprintf(stderr, "rivt: remote session failed: %s\n", err.c_str());
            exit();
            return;
        }
        m_client.attach(sid);
    };

    m_client.on_attach_ok = [this](uint32_t sid) {
        m_session_id = sid;
        m_active = true;
    };

    m_client.on_window_added = [this](uint32_t sid, uint32_t wid) {
        if (sid != m_session_id || m_windows.count(wid)) return;
        Tab *tab = m_tabs.new_empty_tab("rivtd");
        tab->tmux_managed = true;  // pane rects are ours, not the layout engine's
        m_windows[wid] = tab;
        if (m_tabs.on_needs_render) m_tabs.on_needs_render();
    };

    m_client.on_window_closed = [this](uint32_t sid, uint32_t wid) {
        if (sid != m_session_id) return;
        auto it = m_windows.find(wid);
        if (it == m_windows.end()) return;
        Tab *tab = it->second;
        m_windows.erase(it);
        for (auto pit = m_pane_map.begin(); pit != m_pane_map.end();) {
            if (pit->second.wid == wid) {
                m_tabs.remove_pane(tab, pit->second.pane);
                pit = m_pane_map.erase(pit);
            } else {
                ++pit;
            }
        }
        m_tabs.close_tab_ptr(tab);
        // Last window: SessionClosed follows and drives exit().
        if (m_tabs.on_needs_render) m_tabs.on_needs_render();
    };

    m_client.on_layout = [this](uint32_t sid, uint32_t wid, int cols, int rows,
                                const std::vector<RemotePaneGeom> &panes) {
        if (sid != m_session_id) return;
        apply_layout(wid, cols, rows, panes);
    };

    m_client.on_snapshot = [this](uint32_t pane_id, const uint8_t *data, size_t len) {
        auto it = m_pane_map.find(pane_id);
        if (it == m_pane_map.end()) return;
        Pane *p = it->second.pane;
        int cols = p->screen().cols(), rows = p->screen().rows();
        if (!proto::Snapshot::deserialize(p->screen(), p->parser(), data, len)) {
            fprintf(stderr, "rivt: bad snapshot for pane %u\n", pane_id);
            return;
        }
        // The snapshot may predate the layout we already applied; the
        // layout geometry wins.
        if (p->screen().cols() != cols || p->screen().rows() != rows)
            p->resize(cols, rows);
        if (p->on_needs_render) p->on_needs_render();
    };

    m_client.on_output = [this](uint32_t pane_id, const char *data, size_t len) {
        auto it = m_pane_map.find(pane_id);
        if (it != m_pane_map.end()) it->second.pane->feed_data(data, len);
    };

    m_client.on_pane_exited = [this](uint32_t pane_id) {
        auto it = m_pane_map.find(pane_id);
        if (it == m_pane_map.end()) return;
        Pane *pane = it->second.pane;
        auto wit = m_windows.find(it->second.wid);
        m_pane_map.erase(it);
        if (wit != m_windows.end()) m_tabs.remove_pane(wit->second, pane);
        // Window/session lifecycle is driven by WindowClosed/SessionClosed.
        if (m_tabs.on_needs_render) m_tabs.on_needs_render();
    };

    m_client.on_session_closed = [this](uint32_t sid) {
        if (sid == m_session_id) exit();
    };

    m_client.on_disconnect = [this]() {
        fprintf(stderr, "rivt: lost connection to rivtd\n");
        exit();
    };

    m_client.on_error = [](const std::string &e) {
        fprintf(stderr, "rivt: rivtd error: %s\n", e.c_str());
    };
}

void RemoteController::initialize(int cols, int rows, int cell_w, int cell_h,
                                  int content_x, int content_y) {
    m_cols = cols;
    m_rows = rows;
    m_cell_w = cell_w;
    m_cell_h = cell_h;
    m_content_x = content_x;
    m_content_y = content_y;
    m_client.hello();
}

void RemoteController::handle_resize(int cols, int rows, int cell_w, int cell_h,
                                     int content_x, int content_y) {
    m_cols = cols;
    m_rows = rows;
    m_cell_w = cell_w;
    m_cell_h = cell_h;
    m_content_x = content_x;
    m_content_y = content_y;
    if (!m_active) return;
    // The daemon owns the layout: send our new grid; it relayouts and
    // answers with a LayoutUpdate carrying every pane's rect.
    m_client.resize_session(cols, rows);
}

void RemoteController::apply_layout(uint32_t wid, int cols, int rows,
                                    const std::vector<RemotePaneGeom> &panes) {
    auto wit = m_windows.find(wid);
    if (wit == m_windows.end()) return;
    Tab *tab = wit->second;

    // Client grid wins: if the session is sized for someone else,
    // ask for ours (the reply converges in one round).
    if (cols != m_cols || rows != m_rows)
        m_client.resize_session(m_cols, m_rows);

    // Create/update panes present in the layout.
    for (const auto &g : panes) {
        Pane *p;
        auto it = m_pane_map.find(g.id);
        if (it == m_pane_map.end()) {
            p = create_remote_pane(tab, g.id, g.cols, g.rows);
            if (!p) continue;
            m_pane_map[g.id].wid = wid;
        } else {
            p = it->second.pane;
        }
        if (p->screen().cols() != g.cols || p->screen().rows() != g.rows)
            p->resize(g.cols, g.rows);
        p->rect = {m_content_x + g.x * m_cell_w, m_content_y + g.y * m_cell_h,
                   g.cols * m_cell_w, g.rows * m_cell_h};
    }

    // Drop panes this window no longer has.
    for (auto it = m_pane_map.begin(); it != m_pane_map.end();) {
        if (it->second.wid != wid) { ++it; continue; }
        bool present = false;
        for (const auto &g : panes)
            if (g.id == it->first) { present = true; break; }
        if (present) { ++it; continue; }
        Pane *pane = it->second.pane;
        it = m_pane_map.erase(it);
        m_tabs.remove_pane(tab, pane);
    }

    if (m_tabs.on_needs_render) m_tabs.on_needs_render();
}

uint32_t RemoteController::focused_pane_id() const {
    const Tab *tab = m_tabs.active_tab();
    if (!tab || !tab->focused_pane) return 0;
    for (const auto &[id, e] : m_pane_map)
        if (e.pane == tab->focused_pane) return id;
    return 0;
}

void RemoteController::new_window() {
    if (m_active) m_client.new_window();
}

bool RemoteController::request_close_tab(Tab *tab) {
    for (const auto &[wid, t] : m_windows)
        if (t == tab) {
            m_client.close_window(wid);
            return true;
        }
    return false;
}

void RemoteController::split(bool horizontal) {
    uint32_t id = focused_pane_id();
    if (id) m_client.split(id, horizontal);
}

void RemoteController::close_focused_pane() {
    uint32_t id = focused_pane_id();
    if (id) m_client.close_pane(id);
}

Pane *RemoteController::create_remote_pane(Tab *tab, uint32_t pane_id, int cols, int rows) {
    Pane *pane = m_tabs.add_pane_to_tab(tab, cols, rows);
    if (!pane) return nullptr;

    pane->m_write_callback = [this, pane_id](const std::string &data) {
        m_client.send_input(pane_id, data.data(), data.size());
    };
    // Query responses (DA, DSR, ...) are answered once, by the daemon's
    // authoritative parser. The replica must stay silent or every query
    // would be answered twice.
    pane->screen().on_write_back = nullptr;

    m_pane_map[pane_id] = {pane, 0};  // wid filled by caller
    return pane;
}

void RemoteController::detach() {
    if (!m_active) return;
    m_active = false;
    m_client.detach();
}

void RemoteController::exit() {
    m_active = false;
    m_pane_map.clear();
    m_windows.clear();
    if (on_exit) on_exit();
}

} // namespace rivt
