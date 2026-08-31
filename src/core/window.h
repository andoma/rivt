#pragma once
#include "core/config.h"
#include "core/tab_manager.h"
#include "core/input_encoder.h"
#include "render/renderer.h"
#include "net/rendezvous.h"
#include "platform/platform.h"
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace rivt {

class EventLoop;
class TmuxClient;
class TmuxController;
class RemoteClient;
class RemoteController;

class Window {
public:
    Window(const Config &base_config, EventLoop &loop);
    ~Window();

    bool init();
    bool init_tmux_pty(Pane *gateway_pane);
    // Daemon-backed window: attach to session attach_sid, or create one
    // if 0. kill_on_close = throwaway session (dies with a clean close).
    bool init_remote(const std::string &socket_path, uint32_t attach_sid,
                     bool kill_on_close);
    // Daemon on another machine over QUIC (candidate addresses race,
    // first handshake wins). Attaches to the newest session there,
    // creating one if none exist.
    bool init_remote_quic(const std::string &display_name,
                          const std::vector<net::Candidate> &candidates,
                          const std::string &peer_sig_id = "",
                          const std::string &rendezvous = "");
    // Attach a remote session into this (already-initialized) window.
    bool attach_remote(const std::string &display_name,
                       const std::vector<net::Candidate> &candidates,
                       const std::string &peer_sig_id, const std::string &rendezvous);
    // A window that opens showing the device picker (Ctrl-Shift-N).
    bool init_picker(const std::string &rendezvous);
    void render_if_needed();
    bool reap_dead_panes();
    void toggle_cursor_blink();

    Platform *platform() { return m_platform.get(); }
    int event_fd() const { return m_platform->get_event_fd(); }
    bool is_closing() const { return m_closing; }
    // Notifies a remote session before teardown; logs the reason.
    void mark_closing(const char *reason = "unspecified");
    // Log why, then fire on_close (owner marks us closing).
    void request_close(const char *reason);
    // Close the focused pane via its owner (tmux/rivtd/local).
    void close_focused_pane_routed(const char *reason);
    void connectivity_event(Platform::ConnEvent e);  // sleep/wake/path hook
    // While fully obscured the window doesn't render (an occluded X11
    // window's swap can block ~1s, stalling the shared loop), but
    // m_needs_render stays latched so the first frame after it becomes
    // visible again repaints. Swap doesn't block on vsync on X11 (see
    // create_gl_context), so this also paces frames: at most one render
    // per budget interval per window.
    bool needs_render() const {
        if (!m_needs_render || m_platform->is_obscured()) return false;
        return std::chrono::steady_clock::now() - m_last_frame >=
               std::chrono::milliseconds(8);
    }
    TabManager *tab_manager() { return m_tabs.get(); }

    // Resize window to fit a given cell grid (tmux controller; rivtd
    // sessions where the daemon's grid is authoritative)
    void resize_to_cells(int cols, int rows);

    // Grow/shrink the X11 window when the tab bar appears/disappears,
    // keeping the terminal content area unchanged.
    void adjust_tab_bar_height();

    std::function<void()> on_new_window;
    // Picker chose a remote device by name (host to `rivt --connect`).
    std::function<void(const std::string &)> on_pick_remote;
    std::function<void(Pane *gateway)> on_new_tmux_window;
    std::function<void(Window *)> on_close;

    int tab_bar_height() const;

private:
    void setup_callbacks();
    void recompute();
    void update_size_hints();
    void resize_font();
    void handle_key(const KeyEvent &key);
    // Asynchronous paste: the clipboard reply can arrive long after the
    // keystroke, so the target pane is re-validated before it is written to.
    void paste_into(Pane *target, bool primary);
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
    int m_hover_close_tab = -1;  // tab index whose close button is hovered
    // Pane-divider drag (resize). Local tabs adjust the layout tree
    // directly; server-managed tabs send cell deltas to the daemon and
    // wait for LayoutUpdate to move the rects.
    LayoutNode *m_divider_drag = nullptr;
    Pane *m_edge_pane = nullptr;
    bool m_edge_horizontal = false;
    int m_edge_anchor = 0;       // press position along the drag axis
    int m_edge_sent = 0;         // cells already sent this drag
    bool m_resize_cursor = false;

    std::unique_ptr<TmuxClient> m_tmux_client;
    std::unique_ptr<TmuxController> m_tmux_controller;
    std::unique_ptr<RemoteClient> m_remote_client;
    std::unique_ptr<RemoteController> m_remote_controller;
    Pane *m_tmux_gateway_pane = nullptr;  // pane whose PTY carries tmux traffic

    // Attach picker state (a synthetic pane painted with a menu).
    struct PickEntry { std::string label; bool is_local; std::string name; };
    bool m_picker_active = false;
    Pane *m_picker_pane = nullptr;
    std::string m_picker_rendezvous;
    std::vector<net::RosterDevice> m_roster;
    std::vector<PickEntry> m_pick_entries;
    int m_pick_sel = 0;
    std::string m_pick_filter;
    int m_picker_refresh_timer = -1;
    void picker_refresh_roster();
    void picker_rebuild();
    void picker_paint();
    void picker_key(const KeyEvent &key);
    void picker_select();
    void picker_stop();

    // Frame pacing: when the last render finished (see needs_render()).
    std::chrono::steady_clock::time_point m_last_frame{};

    // --debug render stats, summarized once per second (render_if_needed)
    int m_rs_frames = 0;
    double m_rs_build_sum = 0, m_rs_build_max = 0;
    double m_rs_swap_sum = 0, m_rs_swap_max = 0;
    std::chrono::steady_clock::time_point m_rs_last_log{};

    // Deferred destruction — can't destroy while inside feed_data() call stack
    std::unique_ptr<TmuxClient> m_tmux_stale_client;
    std::unique_ptr<TmuxController> m_tmux_stale_controller;
};

} // namespace rivt
