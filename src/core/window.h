#pragma once
#include "core/config.h"
#include "core/tab_manager.h"
#include "core/input_encoder.h"
#include "render/renderer.h"
#include "net/rendezvous.h"
#include "platform/platform.h"
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
    void mark_closing();  // notifies a remote session before teardown
    bool needs_render() const { return m_needs_render; }
    TabManager *tab_manager() { return m_tabs.get(); }

    // Resize window to fit a given cell grid (used by tmux controller)
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
    void picker_rebuild();
    void picker_paint();
    void picker_key(const KeyEvent &key);
    void picker_select();

    // Deferred destruction — can't destroy while inside feed_data() call stack
    std::unique_ptr<TmuxClient> m_tmux_stale_client;
    std::unique_ptr<TmuxController> m_tmux_stale_controller;
};

} // namespace rivt
