#include "remote/remote_controller.h"
#include "core/debug.h"
#include "core/pane.h"
#include "core/tab_manager.h"
#include "core/window.h"
#include "proto/snapshot.h"
#include <algorithm>

namespace rivt {

RemoteController::RemoteController(RemoteClient &client, Window &window, TabManager &tabs)
    : m_client(client), m_window(window), m_tabs(tabs) {

    m_client.on_hello_ok = [this]() {
        if (m_target_sid == ATTACH_NEWEST) {
            m_client.list_sessions();
        } else if (m_target_sid) {
            dbg("remote: attaching to session %u", m_target_sid);
            m_client.attach(m_target_sid);
        } else {
            dbg("remote: creating session (%dx%d)", m_cols, m_rows);
            m_client.create_session("", "", m_cols, m_rows);
        }
    };

    m_client.on_session_list = [this](const std::vector<RemoteSessionInfo> &list) {
        if (m_active || m_target_sid != ATTACH_NEWEST) return;
        if (list.empty()) {
            m_client.create_session("", "", m_cols, m_rows);
        } else {
            m_client.attach(list.back().id);
        }
    };

    m_client.on_session_created = [this](uint32_t sid, uint32_t pane, const std::string &err) {
        if (pane == 0) {
            rivt::logmsg("rivt: remote session failed: %s\n", err.c_str());
            exit();
            return;
        }
        m_client.attach(sid);
    };

    m_client.on_attach_ok = [this](uint32_t sid) {
        m_session_id = sid;
        m_active = true;
        m_sent_cols = m_sent_rows = 0;  // new attach: size not negotiated yet
        if (m_reconnecting) {
            rivt::logmsg("rivt: re-attached to session %u (%s)\n", sid,
                         m_client.transport().c_str());
            m_reconnecting = false;
        } else {
            rivt::logmsg("rivt: attached to session %u (%s)\n", sid,
                         m_client.transport().c_str());
        }
        m_fetching.clear();
        m_fetch_done.clear();
        refresh_status();
    };

    m_client.on_window_added = [this](uint32_t sid, uint32_t wid) {
        if (sid != m_session_id || m_windows.count(wid)) return;
        Tab *tab = m_tabs.new_empty_tab("rivtd");
        tab->tmux_managed = true;  // pane rects are ours, not the layout engine's
        m_windows[wid] = tab;
        reposition_for_tab_bar();
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
        reposition_for_tab_bar();
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
            rivt::logmsg("rivt: bad snapshot for pane %u\n", pane_id);
            return;
        }
        // The snapshot may predate the layout we already applied; the
        // layout geometry wins.
        if (p->screen().cols() != cols || p->screen().rows() != rows)
            p->resize(cols, rows);
        if (p->on_needs_render) p->on_needs_render();
    };

    m_client.on_scrollback = [this](uint32_t pane_id, const uint8_t *data, size_t len) {
        m_fetching.erase(pane_id);
        auto it = m_pane_map.find(pane_id);
        if (it == m_pane_map.end()) return;
        ScreenBuffer &sb = it->second.pane->screen();

        proto::Reader r(data, len);
        uint32_t start = r.u32();
        uint32_t n = r.u32();
        std::vector<Line> lines;
        lines.reserve(n);
        for (uint32_t i = 0; i < n && r.ok; i++) {
            Line l(sb.cols());
            if (!proto::Snapshot::decode_line(r, l)) return;
            lines.push_back(std::move(l));
        }
        if (!r.ok) return;
        // Contiguity: the chunk must end exactly where our history
        // starts. A short/empty or gapped reply means the daemon
        // evicted those lines — stop asking.
        if (n == 0 || start + n != (uint32_t)sb.scrollback_trimmed()) {
            m_fetch_done.insert(pane_id);
            return;
        }
        sb.prepend_scrollback(std::move(lines));
        if (it->second.pane->on_needs_render) it->second.pane->on_needs_render();
        // Still pinned near the top with more available? Keep going.
        if (sb.scrollback_trimmed() > 0 &&
            sb.viewport_offset() <= -(sb.scrollback_count() - 200))
            request_scrollback(pane_id);
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
        if (sid == m_session_id) {
            rivt::logmsg("rivt: session %u closed by rivtd\n", sid);
            exit();
        }
    };

    m_client.on_disconnect = [this]() {
        if (m_reconnecting) {
            // A reconnect attempt's handshake failed asynchronously
            // (QUIC connects resolve after connect() returns) — retry.
            schedule_reconnect_attempt();
            return;
        }
        if (m_active && m_session_id) {
            // A daemon upgrade (exec) drops all sockets but keeps the
            // sessions. Try to reconnect and re-attach before giving up.
            rivt::logmsg("rivt: rivtd connection lost, reconnecting...\n");
            begin_reconnect();
        } else {
            rivt::logmsg("rivt: lost connection to rivtd\n");
            exit();
        }
    };

    m_client.on_error = [this](const std::string &e) {
        rivt::logmsg("rivt: rivtd error: %s\n", e.c_str());
        // During a reconnect, "no such session" means the daemon came
        // back without our state — nothing to re-attach to.
        if (m_reconnecting) exit();
    };

    m_client.on_status = [this]() { refresh_status(); };
    // Re-evaluate staleness periodically (no rx for a while => stale).
    m_status_timer = m_client.loop().add_timer(5000, [this]() { refresh_status(); }, true);
}

RemoteController::~RemoteController() {
    // The EventLoop outlives us (shared): drop any timers we registered,
    // or they'd fire into a freed controller when a window is closed.
    if (m_status_timer >= 0) m_client.loop().remove_timer(m_status_timer);
    if (m_reconnect_timer >= 0) m_client.loop().remove_timer(m_reconnect_timer);
    if (m_resize_timer >= 0) m_client.loop().remove_timer(m_resize_timer);
}

void RemoteController::initialize(int cols, int rows, int cell_w, int cell_h,
                                  int content_x, int content_y, uint32_t target_sid,
                                  bool kill_on_close) {
    m_target_sid = target_sid;
    m_kill_on_close = kill_on_close;
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
    maybe_send_resize();
}

void RemoteController::maybe_send_resize() {
    if (!m_active) return;
    if (m_cols == m_sent_cols && m_rows == m_sent_rows) return;
    if (m_resize_timer >= 0) {
        m_resize_pending = true;  // trailing send when the window closes
        return;
    }
    m_sent_cols = m_cols;
    m_sent_rows = m_rows;
    m_client.resize_session(m_cols, m_rows);
    m_resize_timer = m_client.loop().add_timer(150, [this]() {
        m_client.loop().remove_timer(m_resize_timer);
        m_resize_timer = -1;
        if (m_resize_pending) {
            m_resize_pending = false;
            maybe_send_resize();
        }
    }, true);
}

void RemoteController::apply_layout(uint32_t wid, int cols, int rows,
                                    const std::vector<RemotePaneGeom> &panes) {
    auto wit = m_windows.find(wid);
    if (wit == m_windows.end()) return;
    Tab *tab = wit->second;

    // Client grid wins: if the session is sized for someone else, ask
    // for ours. maybe_send_resize() is a no-op when the current want
    // was already requested, so stale layout updates can't echo.
    if (cols != m_cols || rows != m_rows)
        maybe_send_resize();

    // Create/update panes present in the layout.
    for (const auto &g : panes) {
        if (g.cols < 2 || g.rows < 2) continue;  // degenerate geometry
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
    pane->screen().on_scrollback_wanted = [this, pane_id]() {
        request_scrollback(pane_id);
    };

    m_pane_map[pane_id] = {pane, 0};  // wid filled by caller
    return pane;
}

void RemoteController::notify_window_closing() {
    dbg("remote: window closing (kill_on_close=%d active=%d sid=%u)",
        m_kill_on_close, m_active, m_session_id);
    if (m_kill_on_close && m_active && m_session_id) {
        // Throwaway session: a clean close ends it. send_frame flushes
        // eagerly, so the message leaves before the socket does.
        m_client.kill_session(m_session_id);
    }
    m_active = false;
}

void RemoteController::detach() {
    if (!m_active) return;
    m_active = false;
    m_client.detach();
}

// The tab bar appears at 2 tabs and disappears at 1. Grow/shrink the
// window so the terminal grid is unchanged, and shift existing pane
// rects by the content-origin delta (same dance as TmuxController).
void RemoteController::connectivity_event(Platform::ConnEvent e) {
    using CE = Platform::ConnEvent;
    switch (e) {
    case CE::Sleep:
        // Sticky: nothing runs (no probes, no reconnects, no relay
        // flows burned by dark-wake churn) until a real Wake.
        m_asleep = true;
        park();
        break;
    case CE::PathDown:
        park();
        break;
    case CE::Wake:
        m_asleep = false;
        [[fallthrough]];
    case CE::PathUp:
        if (m_asleep) break;  // dark-wake path noise: stay parked
        if (m_reconnecting) {
            // Fresh budget: the outage was sleep or a network change,
            // not the peer being gone.
            rivt::logmsg("rivt: resuming reconnect (budget reset)\n");
            m_reconnect_attempts = 0;
            schedule_reconnect_attempt();
        } else if (m_active) {
            m_client.verify_link();
        }
        break;
    }
    refresh_status();
}

void RemoteController::park() {
    m_client.suspend_probes();
    if (m_reconnect_timer >= 0) {
        m_client.loop().remove_timer(m_reconnect_timer);
        m_reconnect_timer = -1;
    }
}

void RemoteController::begin_reconnect() {
    m_active = false;
    m_target_sid = m_session_id;  // hello_ok re-attaches to the same session
    m_reconnecting = true;
    m_reconnect_attempts = 0;
    refresh_status();
    schedule_reconnect_attempt();
}

void RemoteController::refresh_status() {
    std::string t = m_client.transport();
    std::string state;
    if (m_reconnecting) state = "reconnecting";
    else if (!m_active) state = "connecting";
    else if (m_client.seconds_since_rx() > 45) state = "stale";
    std::string s = m_peer_name.empty() ? "remote" : m_peer_name;
    if (!t.empty()) s += "  \u00b7  " + t;   // middle dot
    if (!state.empty()) s += "  \u00b7  " + state;
    m_tabs.set_title_suffix(s);
}

void RemoteController::schedule_reconnect_attempt() {
    if (m_asleep) return;  // parked; Wake resumes via connectivity_event
    if (m_reconnect_timer >= 0) return;  // one pending attempt at a time
    // Roaming laptops disappear for a while (network switch, lid close,
    // captive portal); keep trying for several minutes, backing off from
    // 0.5s to 5s between attempts, before declaring the session gone.
    if (m_reconnect_attempts >= 40) {
        m_reconnecting = false;
        rivt::logmsg("rivt: rivtd did not come back\n");
        exit();
        return;
    }
    int delay = std::min(500 * (1 + m_reconnect_attempts), 5000);
    m_reconnect_timer = m_client.loop().add_timer(delay, [this]() {
        m_client.loop().remove_timer(m_reconnect_timer);
        m_reconnect_timer = -1;
        if (!m_reconnecting) return;
        m_reconnect_attempts++;
        dbg("rivt: reconnect attempt %d/10", m_reconnect_attempts);
        // No autostart while the daemon execs — a race would spawn a
        // fresh empty daemon over the upgrading one. For QUIC, a true
        // return only means the handshake started; failures arrive via
        // on_disconnect, which re-schedules.
        if (m_client.reconnect())
            m_client.hello();
        else
            schedule_reconnect_attempt();
    }, true);
}

void RemoteController::request_scrollback(uint32_t pane_id) {
    if (!m_active || m_fetching.count(pane_id) || m_fetch_done.count(pane_id)) return;
    auto it = m_pane_map.find(pane_id);
    if (it == m_pane_map.end()) return;
    int trimmed = it->second.pane->screen().scrollback_trimmed();
    if (trimmed <= 0) return;
    m_fetching.insert(pane_id);
    m_client.fetch_scrollback(pane_id, (uint32_t)trimmed, 500);
}

void RemoteController::reposition_for_tab_bar() {
    int new_y = m_window.tab_bar_height();
    if (new_y == m_content_y) return;
    int delta = new_y - m_content_y;
    m_content_y = new_y;
    m_window.adjust_tab_bar_height();
    for (auto &[id, e] : m_pane_map)
        e.pane->rect.y += delta;
}

void RemoteController::exit() {
    m_active = false;
    m_pane_map.clear();
    m_windows.clear();
    m_fetching.clear();
    m_fetch_done.clear();
    if (m_status_timer >= 0) { m_client.loop().remove_timer(m_status_timer); m_status_timer = -1; }
    m_tabs.set_title_suffix("");
    if (on_exit) on_exit();
}

} // namespace rivt
