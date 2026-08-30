#pragma once
#include "remote/remote_client.h"
#include "platform/platform.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

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
    ~RemoteController();

    static constexpr uint32_t ATTACH_NEWEST = 0xffffffff;

    // Attach to session target_sid, a fresh session if 0, or the newest
    // existing session (creating one if none) with ATTACH_NEWEST.
    // cols/rows: our window's grid; the daemon layout is sized to it.
    // kill_on_close: throwaway session — a clean window close kills it
    // (plain rivt semantics). A crash sends nothing, so it survives.
    void initialize(int cols, int rows, int cell_w, int cell_h,
                    int content_x, int content_y, uint32_t target_sid,
                    bool kill_on_close);

    // Called by Window on a clean close (close button, quit).
    void notify_window_closing();

    // Display name of the peer, for the window-title status.
    void set_peer_name(const std::string &name) { m_peer_name = name; }
    void handle_resize(int cols, int rows, int cell_w, int cell_h,
                       int content_x, int content_y);

    bool is_active() const { return m_active; }
    void detach();

    // User actions, routed to the daemon.
    void split(bool horizontal);
    void close_focused_pane();
    void new_window();
    // Divider drag on a remote pane: move its right/bottom edge by
    // delta cells (daemon relayouts; LayoutUpdate brings new rects).
    void resize_pane_edge(Pane *pane, bool horizontal, int delta_cells);
    // Returns true if the tab is remote-managed and its close was
    // requested from the daemon (teardown happens on WindowClosed).
    bool request_close_tab(Tab *tab);

    // Sleep parks all remote maintenance (sticky: dark-wake path events
    // must not resume it); Wake resumes with a fresh reconnect budget;
    // PathUp nudges an awake client; PathDown parks probes non-stickily.
    void connectivity_event(Platform::ConnEvent e);

    // Fired when the session ends or the daemon goes away.
    std::function<void()> on_exit;

private:
    Pane *create_remote_pane(Tab *tab, uint32_t pane_id, int cols, int rows);
    void apply_layout(uint32_t wid, int cols, int rows,
                      const std::vector<RemotePaneGeom> &panes);
    uint32_t focused_pane_id() const;
    void request_scrollback(uint32_t pane_id);
    void reposition_for_tab_bar();
    void refresh_status();
    void exit();
    void park();  // stop probes + pending reconnect attempts

    RemoteClient &m_client;
    Window &m_window;
    TabManager &m_tabs;

    struct PaneEntry { Pane *pane; uint32_t wid; };

    bool m_active = false;
    bool m_kill_on_close = false;
    uint32_t m_target_sid = 0;
    uint32_t m_session_id = 0;
    std::unordered_map<uint32_t, Tab *> m_windows;     // wid -> tab
    std::unordered_map<uint32_t, PaneEntry> m_pane_map;
    std::unordered_set<uint32_t> m_fetching;      // scrollback fetch in flight
    std::unordered_set<uint32_t> m_fetch_done;    // daemon has no more history

    // Resize coalescing: interactive drags emit hundreds of resize
    // events; send at most one in flight per debounce window, and never
    // re-send dimensions already requested (stale LayoutUpdates would
    // otherwise echo into a resize storm).
    void maybe_send_resize();
    int m_resize_timer = -1;
    bool m_resize_pending = false;
    int m_sent_cols = 0, m_sent_rows = 0;

    // Reconnect after daemon restart/upgrade (sessions survive an exec).
    void begin_reconnect();
    void schedule_reconnect_attempt();
    bool m_reconnecting = false;
    bool m_asleep = false;  // sticky until a real Wake event
    int m_reconnect_timer = -1;
    int m_reconnect_attempts = 0;
    std::string m_peer_name;
    std::string m_last_suffix;
    int m_status_timer = -1;

    // Predictive echo. Every prediction is scored against the
    // authoritative screen on PANE_ACK; speculative glyphs are only
    // *drawn* (underlined, via ScreenBuffer::predictions) once the
    // confidence gate opens. RIVT_PREDICT=off|on|auto (default auto:
    // show after a confirmed streak when confirm latency is worth it).
    struct ShadowPred {
        uint32_t seq;
        int row, col;
        uint32_t ch;
        std::chrono::steady_clock::time_point at;
    };
    struct PanePredict {
        std::vector<ShadowPred> preds;
        int streak = 0;
        bool showing = false;
        bool remote_echo_off = false;
    };
    std::unordered_map<uint32_t, PanePredict> m_shadow;
    struct {
        uint64_t keys = 0, predicted = 0, hits = 0, miss_cell = 0,
                 miss_timeout = 0;
        double latency_sum_ms = 0;
        double confirm_ewma_ms = 0;
    } m_shadow_stats;
    void shadow_predict(uint32_t pane_id, Pane *pane, const std::string &data,
                        uint32_t seq);
    void shadow_ack(uint32_t pane_id, uint32_t seq, bool echo_off);
    void shadow_expire(uint32_t pane_id, PanePredict &ps);
    void predict_reset(uint32_t pane_id, PanePredict &ps, const char *why);
    void predict_overlay_sync(uint32_t pane_id, const PanePredict &ps);

    // Per-pane streams have no ordering against control (stream 0): a
    // snapshot can arrive before the LayoutUpdate that creates its pane.
    // Park early pane frames and replay them on pane creation.
    struct EarlyFrame { int kind; std::string data; };  // 0=snapshot 1=output 2=scrollback
    std::unordered_map<uint32_t, std::vector<EarlyFrame>> m_early;
    static constexpr size_t EARLY_MAX = 8 << 20;

    int m_cols = 80, m_rows = 24;
    int m_cell_w = 0, m_cell_h = 0;
    int m_content_x = 0, m_content_y = 0;
};

} // namespace rivt
