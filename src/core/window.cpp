#include "core/window.h"
#include "core/event_loop.h"
#include "core/debug.h"
#include "tmux/tmux_client.h"
#include "tmux/tmux_controller.h"
#include "remote/remote_client.h"
#include "remote/remote_controller.h"

#include "platform/keysym.h"
#include <algorithm>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <spawn.h>

extern "C" { extern char **environ; }

namespace rivt {

// Extra pixels below the last row so descenders/underscores aren't clipped
static constexpr int kBottomPad = 2;

Window::Window(const Config &base_config, EventLoop &loop)
    : m_config(base_config), m_loop(loop) {}

Window::~Window() {
    // Drop our loop timer (shared EventLoop outlives the window).
    if (m_picker_refresh_timer >= 0) m_loop.remove_timer(m_picker_refresh_timer);
    // Replace the gateway pane's override with a drain that swallows remaining
    // tmux protocol (up to the DCS terminator \033\\) before restoring normal
    // operation. Don't touch on_tmux_control_mode — it stays active so the
    // user can run tmux -CC again.
    if (m_tmux_gateway_pane) {
        Pane *gw = m_tmux_gateway_pane;
        gw->m_pty_data_override = [gw](const char *buf, int len) {
            // Swallow tmux protocol. Look for ST (\033\\) which terminates
            // the DCS that started CC mode — everything after it is normal.
            std::string_view sv(buf, len);
            auto pos = sv.find("\033\\");
            if (pos != std::string_view::npos) {
                gw->m_pty_data_override = nullptr;
                // Feed any data after ST to the pane normally
                const char *after = buf + pos + 2;
                int remaining = len - (int)(pos + 2);
                if (remaining > 0) {
                    gw->feed_data(after, remaining);
                }
            }
        };
        m_tmux_gateway_pane = nullptr;
    }
}

bool Window::init() {
    m_platform = Platform::create();
    if (!m_platform) {
        rivt::logmsg("Failed to create platform\n");
        return false;
    }

    if (!m_platform->create_window(m_win_w, m_win_h, "rivt")) {
        rivt::logmsg("Failed to create window\n");
        return false;
    }

    if (!m_platform->create_gl_context()) {
        rivt::logmsg("Failed to create GL context\n");
        return false;
    }

    if (!m_renderer.init(m_config, m_platform->get_dpi_scale() * 96.0f)) {
        rivt::logmsg("Failed to initialize renderer\n");
        return false;
    }

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        request_close("last tab closed");
    };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height + kBottomPad;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);

    if (!m_tabs->new_tab()) {
        rivt::logmsg("Failed to spawn initial shell\n");
        return false;
    }
    recompute();

    m_platform->show_window();
    setup_callbacks();
    return true;
}

int Window::tab_bar_height() const {
    const auto &m = m_renderer.metrics();
    return m_tabs->tab_count() > 1 ? m.cell_height + 8 : 0;
}

void Window::recompute() {
    adjust_tab_bar_height();
    int bar_h = tab_bar_height();
    m_tabs->recompute_layout(0, bar_h, m_win_w, m_win_h - bar_h);
    update_size_hints();
}

void Window::adjust_tab_bar_height() {
    int bar_h = tab_bar_height();
    if (bar_h == m_last_bar_h) return;
    int delta = bar_h - m_last_bar_h;
    m_last_bar_h = bar_h;
    m_win_h += delta;
    dbg("window: tab bar %s (%d -> %d px), window now %dx%d",
        delta > 0 ? "appeared" : "disappeared", bar_h - delta, bar_h, m_win_w, m_win_h);
    // Update size hints first so the WM knows the new base size before
    // we request the resize — otherwise it may snap to the wrong grid.
    update_size_hints();
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);
}

void Window::update_size_hints() {
    const auto &m = m_renderer.metrics();
    if (m.cell_width > 0 && m.cell_height > 0) {
        int bar_h = tab_bar_height();
        m_platform->set_size_hints(m.cell_width, m.cell_height, 0, bar_h + kBottomPad);
    }
}

void Window::resize_to_cells(int cols, int rows) {
    const auto &m = m_renderer.metrics();
    int bar_h = tab_bar_height();
    m_win_w = cols * m.cell_width;
    m_win_h = rows * m.cell_height + bar_h + kBottomPad;
    m_last_bar_h = bar_h;
    dbg("window(%p): resize_to_cells %dx%d -> %dx%d px (bar_h=%d cell=%dx%d)",
        (void*)this, cols, rows, m_win_w, m_win_h, bar_h, m.cell_width, m.cell_height);
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);
    recompute();
    m_needs_render = true;
}

void Window::resize_font() {
    int cols, rows;
    m_renderer.compute_grid(m_win_w, m_win_h - m_last_bar_h - kBottomPad, cols, rows);
    m_renderer.set_font_size(m_config.font_size, m_platform->get_dpi_scale() * 96.0f);
    m_tabs->set_cell_size(m_renderer.metrics().cell_width, m_renderer.metrics().cell_height);
    const auto &met = m_renderer.metrics();
    int bar_h = tab_bar_height();
    m_win_w = cols * met.cell_width;
    m_win_h = rows * met.cell_height + bar_h + kBottomPad;
    m_last_bar_h = bar_h;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);
    recompute();
    m_needs_render = true;
}

void Window::toggle_cursor_blink() {
    m_cursor_blink_on = !m_cursor_blink_on;
    m_needs_render = true;
}

bool Window::reap_dead_panes() {
    if (!m_tabs->reap_dead_panes()) {
        return false;
    }
    recompute();
    m_needs_render = true;
    return true;
}

void Window::setup_callbacks() {
    m_platform->on_key = [this](const KeyEvent &key) { handle_key(key); };
    m_platform->on_mouse = [this](const MouseEvent &mouse) { handle_mouse(mouse); };
    m_platform->on_resize = [this](int w, int h) { handle_resize(w, h); };

    m_platform->on_focus = [this](bool focused) {
        m_focused = focused;
        m_renderer.set_focused(focused);
        m_needs_render = true;
        Pane *pane = m_tabs->focused_pane();
        if (pane && pane->screen().focus_reporting()) {
            pane->write(focused ? "\033[I" : "\033[O");
        }
    };

    m_platform->on_close = [this]() {
        if (m_tmux_controller && m_tmux_controller->is_active()) {
            m_tmux_controller->detach();
        }
        mark_closing("window manager close");  // notifies remote (kill-on-clean-close)
    };

    // Native menu actions (Cmd-N / Cmd-W / Cmd-C / Cmd-V on macOS).
    // No-op on platforms without a menu bar; Ctrl-Shift-* shortcuts in
    // handle_key still cover Linux.
    m_platform->on_menu_new_window = [this]() {
        if (on_new_window) on_new_window();
    };
    m_platform->on_menu_close_window = [this]() {
        close_focused_pane_routed("menu close, last pane");
    };
    m_platform->on_menu_copy = [this]() {
        Pane *pane = m_tabs ? m_tabs->focused_pane() : nullptr;
        if (!pane) return;
        std::string text = pane->screen().get_selection_text();
        if (!text.empty()) m_platform->set_clipboard(text, false);
    };
    m_platform->on_menu_paste = [this]() {
        paste_into(m_tabs ? m_tabs->focused_pane() : nullptr, false);
    };

    // Detect tmux -CC control mode in any pane's PTY output
    m_tabs->on_tmux_control_mode = [this](Pane *pane) {
        start_tmux_from_pane(pane);
    };
}

void Window::start_tmux_from_pane(Pane *gateway) {
    dbg("window: start_tmux_from_pane gateway=%p", (void*)gateway);
    if (on_new_tmux_window) on_new_tmux_window(gateway);
}

bool Window::init_tmux_pty(Pane *gateway_pane) {
    dbg("window: init_tmux_pty gateway=%p", (void*)gateway_pane);
    m_platform = Platform::create();
    if (!m_platform) return false;
    if (!m_platform->create_window(m_win_w, m_win_h, "rivt [tmux]")) return false;
    if (!m_platform->create_gl_context()) return false;
    if (!m_renderer.init(m_config, m_platform->get_dpi_scale() * 96.0f)) return false;

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        request_close("last tab closed");
    };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tmux_gateway_pane = gateway_pane;

    // Create tmux client in PTY mode — writes go to the gateway pane's PTY
    m_tmux_client = std::make_unique<TmuxClient>(m_loop);
    m_tmux_client->start_pty_mode([gateway_pane](const std::string &data) {
        // Route through Pane::write (not pty().write) so the queued-write
        // flush interest is armed — large send-keys lines from a paste can
        // exceed the PTY buffer and must be drained on writable.
        gateway_pane->write(data);
    });

    m_tmux_controller = std::make_unique<TmuxController>(*m_tmux_client, *this, *m_tabs, m_loop);
    m_tmux_controller->set_gateway_pane(gateway_pane);
    m_tmux_controller->on_tmux_exit = [this]() {
        stop_tmux_pty_mode();
    };

    // Redirect gateway pane's PTY reads to our tmux client
    gateway_pane->m_pty_data_override = [this](const char *buf, int len) {
        m_tmux_client->feed_data(buf, len);
    };

    int bar_h = tab_bar_height();
    int cols = m.cell_width > 0 ? m_win_w / m.cell_width : 80;
    int rows = m.cell_height > 0 ? (m_win_h - bar_h - kBottomPad) / m.cell_height : 24;
    m_tmux_controller->initialize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);

    m_platform->show_window();
    setup_callbacks();
    return true;
}

bool Window::init_remote(const std::string &socket_path, uint32_t attach_sid,
                         bool kill_on_close) {
    m_platform = Platform::create();
    if (!m_platform) return false;
    if (!m_platform->create_window(m_win_w, m_win_h, "rivt [rivtd]")) return false;
    if (!m_platform->create_gl_context()) return false;
    if (!m_renderer.init(m_config, m_platform->get_dpi_scale() * 96.0f)) return false;

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        request_close("last tab closed");
    };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height + kBottomPad;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);

    m_remote_client = std::make_unique<RemoteClient>(m_loop);
    std::string path = socket_path.empty() ? RemoteClient::default_socket_path()
                                           : socket_path;
    if (!m_remote_client->connect(path, /*autostart=*/true)) {
        rivt::logmsg("rivt: cannot reach rivtd at %s\n", path.c_str());
        return false;
    }

    m_remote_controller = std::make_unique<RemoteController>(*m_remote_client, *this, *m_tabs);
    m_remote_controller->on_exit = [this]() {
        request_close("remote session ended");
    };

    int bar_h = tab_bar_height();
    int cols = m.cell_width > 0 ? m_win_w / m.cell_width : 80;
    int rows = m.cell_height > 0 ? (m_win_h - bar_h - kBottomPad) / m.cell_height : 24;
    m_remote_controller->initialize(cols, rows, m.cell_width, m.cell_height, 0, bar_h,
                                    attach_sid, kill_on_close);

    m_platform->show_window();
    setup_callbacks();
    return true;
}

// Close the focused pane through whoever owns it. tmux and rivtd panes
// are closed daemon-side — teardown arrives later as protocol events
// that keep the controller's bookkeeping consistent. Freeing them
// locally (as the Cmd-W menu path once did) leaves dangling Pane
// pointers in the controller maps: heap-use-after-free on the next
// layout/window message.
void Window::close_focused_pane_routed(const char *reason) {
    if (m_tmux_controller && m_tmux_controller->is_active()) {
        m_tmux_client->send_command("kill-pane");
    } else if (m_remote_controller && m_remote_controller->is_active()) {
        m_remote_controller->close_focused_pane();
    } else if (m_tabs && !m_tabs->close_focused_pane()) {
        request_close(reason);
    }
    m_needs_render = true;
}

void Window::request_close(const char *reason) {
    rivt::logmsg("window(%p): close requested (%s)\n", (void *)this, reason);
    if (on_close) on_close(this);
}

void Window::mark_closing(const char *reason) {
    if (m_closing) return;
    // A window disappearing must never be silent — always say why.
    rivt::logmsg("window(%p): closing (%s)\n", (void *)this, reason);
    if (m_remote_controller) m_remote_controller->notify_window_closing();
    m_closing = true;
}

bool Window::init_remote_quic(const std::string &display_name,
                              const std::vector<net::Candidate> &candidates,
                              const std::string &peer_sig_id,
                              const std::string &rendezvous) {
    m_platform = Platform::create();
    if (!m_platform) return false;
    std::string title = "rivt [" + display_name + "]";
    if (!m_platform->create_window(m_win_w, m_win_h, title.c_str())) return false;
    if (!m_platform->create_gl_context()) return false;
    if (!m_renderer.init(m_config, m_platform->get_dpi_scale() * 96.0f)) return false;

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        request_close("last tab closed");
    };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height + kBottomPad;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);

    if (!attach_remote(display_name, candidates, peer_sig_id, rendezvous)) return false;

    m_platform->show_window();
    setup_callbacks();
    return true;
}

bool Window::attach_remote(const std::string &display_name,
                           const std::vector<net::Candidate> &candidates,
                           const std::string &peer_sig_id, const std::string &rendezvous) {
    m_remote_client = std::make_unique<RemoteClient>(m_loop);
    RemoteEndpoint ep;
    ep.candidates = candidates;
    ep.peer_sig_id = peer_sig_id;
    ep.rendezvous = rendezvous;
    if (!m_remote_client->connect(ep, false)) {
        rivt::logmsg("rivt: cannot reach %s\n", display_name.c_str());
        return false;
    }
    m_remote_controller = std::make_unique<RemoteController>(*m_remote_client, *this, *m_tabs);
    m_remote_controller->set_peer_name(display_name);
    m_remote_controller->on_exit = [this]() {
        request_close("remote session ended");
    };
    const auto &m = m_renderer.metrics();
    int bar_h = tab_bar_height();
    int cols = m.cell_width > 0 ? m_win_w / m.cell_width : 80;
    int rows = m.cell_height > 0 ? (m_win_h - bar_h - kBottomPad) / m.cell_height : 24;
    m_remote_controller->initialize(cols, rows, m.cell_width, m.cell_height, 0, bar_h,
                                    RemoteController::ATTACH_NEWEST, /*kill_on_close=*/false);
    return true;
}

bool Window::init_picker(const std::string &rendezvous) {
    m_platform = Platform::create();
    if (!m_platform) return false;
    if (!m_platform->create_window(m_win_w, m_win_h, "rivt")) return false;
    if (!m_platform->create_gl_context()) return false;
    if (!m_renderer.init(m_config, m_platform->get_dpi_scale() * 96.0f)) return false;
    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() { request_close("last tab closed"); };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height + kBottomPad;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);
    recompute();

    m_picker_rendezvous = rendezvous;
    m_picker_pane = m_tabs->new_synthetic_tab("connect");
    if (!m_picker_pane) return false;
    m_picker_active = true;
    // Fetch the roster (blocking; small). Then paint the menu.
    if (!rendezvous.empty()) net::list_devices(rendezvous, m_roster);
    picker_rebuild();
    picker_paint();
    // Refresh the roster periodically so online dots stay current (the
    // one-shot snapshot goes stale as its last_seen values age out).
    if (!rendezvous.empty())
        m_picker_refresh_timer = m_loop.add_timer(15000, [this]() { picker_refresh_roster(); }, true);

    m_platform->show_window();
    setup_callbacks();
    return true;
}

void Window::picker_rebuild() {
    m_pick_entries.clear();
    auto matches = [&](const std::string &n) {
        if (m_pick_filter.empty()) return true;
        return n.find(m_pick_filter) != std::string::npos;
    };
    if (matches("local terminal") || m_pick_filter.empty())
        m_pick_entries.push_back({"local terminal", true, ""});
    for (const auto &d : m_roster)
        if (matches(d.name)) m_pick_entries.push_back({d.name, false, d.name});
    if (m_pick_sel >= (int)m_pick_entries.size()) m_pick_sel = (int)m_pick_entries.size() - 1;
    if (m_pick_sel < 0) m_pick_sel = 0;
}

void Window::picker_paint() {
    if (!m_picker_pane) return;
    int64_t now = (int64_t)time(nullptr) * 1000;
    std::string o = "\033[2J\033[H\r\n";
    o += "  \033[1mrivt\033[0m  \033[2m\u2014 connect\033[0m\r\n\r\n";
    for (int i = 0; i < (int)m_pick_entries.size(); i++) {
        const auto &e = m_pick_entries[i];
        const std::string &label = e.label;
        bool online = false;
        if (!e.is_local)
            for (const auto &d : m_roster)
                if (d.name == e.name && now - d.last_seen_ms < 90000) online = true;
        // Marker glyph occupies one column; label is ASCII. Pad the row
        // so the highlight bar is a clean rectangle.
        const char *glyph = e.is_local ? " " : (online ? "\u25cf" : "\u25cb");
        int width = 2 + (int)label.size();  // glyph + space + label
        std::string pad(width < 28 ? 28 - width : 0, ' ');
        if (i == m_pick_sel) {
            // Reverse video spans the whole row: no color resets inside,
            // or they'd clear the reverse attribute mid-line.
            o += "  \033[7m" + std::string(glyph) + " " + label + pad + "\033[0m\r\n";
        } else {
            std::string cglyph = e.is_local ? " "
                : (online ? "\033[32m\u25cf\033[0m" : "\033[2m\u25cb\033[0m");
            o += "  " + cglyph + " " + label + "\r\n";
        }
    }
    o += "\r\n  \033[2m\u2191\u2193 select · enter connect · type to filter · esc close\033[0m";
    if (!m_pick_filter.empty()) o += "\r\n\r\n  filter: " + m_pick_filter;
    m_picker_pane->feed_data(o.data(), o.size());
    m_needs_render = true;
}

void Window::picker_key(const KeyEvent &key) {
    bool ctrl = key.mods & KeyMod::Ctrl;
    if (key.keysym == XKB_KEY_Escape) { request_close("picker: escape"); return; }
    if (key.keysym == XKB_KEY_Up || (ctrl && (key.keysym == XKB_KEY_p || key.keysym == XKB_KEY_P))) {
        if (m_pick_sel > 0) m_pick_sel--;
        picker_paint();
        return;
    }
    if (key.keysym == XKB_KEY_Down || (ctrl && (key.keysym == XKB_KEY_n || key.keysym == XKB_KEY_N))) {
        if (m_pick_sel < (int)m_pick_entries.size() - 1) m_pick_sel++;
        picker_paint();
        return;
    }
    if (key.keysym == XKB_KEY_Return) { picker_select(); return; }
    if (key.keysym == XKB_KEY_Delete) {  // Delete removes the selected device
        if (m_pick_sel >= 0 && m_pick_sel < (int)m_pick_entries.size()) {
            const PickEntry &e = m_pick_entries[m_pick_sel];
            if (!e.is_local && !m_picker_rendezvous.empty()) {
                auto id = net::Identity::load_or_create();
                if (id) net::delete_device(m_picker_rendezvous, *id, e.name);
                m_roster.erase(std::remove_if(m_roster.begin(), m_roster.end(),
                    [&](const net::RosterDevice &d) { return d.name == e.name; }),
                    m_roster.end());
                picker_rebuild();
                picker_paint();
            }
        }
        return;
    }
    if (key.keysym == XKB_KEY_BackSpace) {
        if (!m_pick_filter.empty()) m_pick_filter.pop_back();
        picker_rebuild(); picker_paint(); return;
    }
    // Printable text filters the list.
    if (!key.text.empty() && (unsigned char)key.text[0] >= 0x20) {
        m_pick_filter += key.text;
        picker_rebuild(); picker_paint(); return;
    }
}

void Window::picker_refresh_roster() {
    if (!m_picker_active || m_picker_rendezvous.empty()) return;
    std::vector<net::RosterDevice> fresh;
    if (net::list_devices(m_picker_rendezvous, fresh)) {
        m_roster = std::move(fresh);
        picker_rebuild();
        picker_paint();
    }
}

void Window::picker_stop() {
    m_picker_active = false;
    if (m_picker_refresh_timer >= 0) {
        m_loop.remove_timer(m_picker_refresh_timer);
        m_picker_refresh_timer = -1;
    }
}

void Window::picker_select() {
    if (m_pick_sel < 0 || m_pick_sel >= (int)m_pick_entries.size()) return;
    PickEntry e = m_pick_entries[m_pick_sel];
    picker_stop();
    if (e.is_local) {
        // Turn the picker pane into a real local terminal in place.
        // Reset attributes, clear screen + scrollback, home the cursor
        // (the length was wrong before, leaving the cursor mid-screen).
        static const char kReset[] = "\033[0m\033[2J\033[3J\033[H";
        m_picker_pane->feed_data(kReset, sizeof kReset - 1);
        m_picker_pane->spawn_shell(m_loop);
        m_picker_pane = nullptr;
        m_needs_render = true;
    } else {
        // Hand off to a fresh remote window; close the picker.
        std::string name = e.name;
        if (on_pick_remote) on_pick_remote(name);
        mark_closing("picker: handing off to remote window");
    }
}


void Window::stop_tmux_pty_mode() {
    if (!m_tmux_gateway_pane) return;

    // Restore gateway pane to normal operation
    m_tmux_gateway_pane->m_pty_data_override = nullptr;
    m_tmux_gateway_pane = nullptr;

    // Defer destruction — we're likely inside m_tmux_client->feed_data() call stack
    m_tmux_stale_controller = std::move(m_tmux_controller);
    m_tmux_stale_client = std::move(m_tmux_client);

    // Close this tmux window
    m_closing = true;
}

void Window::connectivity_event(Platform::ConnEvent e) {
    if (m_remote_controller) m_remote_controller->connectivity_event(e);
}

void Window::handle_resize(int w, int h) {
    dbg("window(%p): handle_resize %dx%d tmux=%d gateway=%p",
        (void*)this, w, h,
        m_tmux_controller && m_tmux_controller->is_active(),
        (void*)m_tmux_gateway_pane);
    m_win_w = w;
    m_win_h = h;
    // The window may have moved to a screen with a different backing
    // scale (retina <-> non-retina); the font must be re-rasterized at
    // the new DPI or the grid comes out at the old scale.
    float want_dpi = m_platform->get_dpi_scale() * 96.0f;
    if (want_dpi != m_renderer.font().dpi()) {
        // If this fires on every resize event the DPI values never
        // converge (float rounding) and each ConfigureNotify pays a full
        // font re-rasterization + atlas clear.
        dbg("window(%p): dpi %.4f -> %.4f, re-rasterizing font (atlas cleared)",
            (void *)this, (double)m_renderer.font().dpi(), (double)want_dpi);
        m_renderer.set_font_size(m_config.font_size, want_dpi);
    }
    m_last_bar_h = tab_bar_height();
    m_renderer.set_viewport(w, h);
    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);

    if (m_tmux_controller && m_tmux_controller->is_active()) {
        int bar_h = m_last_bar_h;
        int cols = m.cell_width > 0 ? w / m.cell_width : 80;
        int rows = m.cell_height > 0 ? (h - bar_h - kBottomPad) / m.cell_height : 24;
        dbg("window(%p): tmux resize -> %dx%d cells (bar_h=%d)", (void*)this, cols, rows, bar_h);
        m_tmux_controller->handle_resize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);
    }

    if (m_remote_controller && m_remote_controller->is_active()) {
        int bar_h = m_last_bar_h;
        int cols = m.cell_width > 0 ? w / m.cell_width : 80;
        int rows = m.cell_height > 0 ? (h - bar_h - kBottomPad) / m.cell_height : 24;
        m_remote_controller->handle_resize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);
    }

    recompute();
    m_needs_render = true;
}

void Window::paste_into(Pane *target, bool primary) {
    if (!target) return;
    m_platform->request_clipboard(primary, [this, target, alive = target->alive_token()]
                                          (const std::string &text) {
        if (text.empty() || alive.expired()) return;
        if (target->screen().bracketed_paste()) {
            target->write("\033[200~");
            target->write(text);
            target->write("\033[201~");
        } else {
            target->write(text);
        }
        m_needs_render = true;
    });
}

void Window::handle_key(const KeyEvent &raw_key) {
    if (!raw_key.pressed) return;

    Pane *pane = m_tabs->focused_pane();
    if (!pane) return;

    // On macOS, treat Cmd (Super) as the canonical rivt-shortcut modifier
    // — equivalent to Ctrl+Shift on Linux. This makes Cmd-F open search,
    // Cmd-+/- resize font, Cmd-T open a tab, Cmd-D split a pane, etc.
    // Cmd-N/W/C/V/Q are already routed via the native menu bar.
    KeyEvent key = raw_key;
#ifdef __APPLE__
    bool macos_cmd_held = (key.mods & KeyMod::Super);
    if (macos_cmd_held) key.mods = key.mods | KeyMod::Ctrl | KeyMod::Shift;
#else
    constexpr bool macos_cmd_held = false;
#endif

    if (m_picker_active) { picker_key(key); return; }

    ScreenBuffer &screen = pane->screen();
    bool ctrl  = key.mods & KeyMod::Ctrl;
    bool shift = key.mods & KeyMod::Shift;

    // Search mode input handling
    if (screen.search.focused) {
        switch (key.keysym) {
            case XKB_KEY_Escape:
                screen.search.focused = false;
                m_needs_render = true;
                return;
            case XKB_KEY_Return: {
                // search_navigate
                auto &s = screen.search;
                if (!s.matches.empty()) {
                    int delta = shift ? 1 : -1;
                    s.current_match = (s.current_match + delta + (int)s.matches.size()) % (int)s.matches.size();
                    const auto &match = s.matches[s.current_match];
                    int rows = screen.rows();
                    int vis_row = match.abs_line - screen.absolute_line(0);
                    if (vis_row < 0 || vis_row >= rows) {
                        int base = screen.absolute_line(0) - screen.viewport_offset();
                        int target_offset = match.abs_line - base - rows / 2;
                        screen.scroll_viewport(target_offset - screen.viewport_offset());
                    }
                    m_needs_render = true;
                }
                return;
            }
            case XKB_KEY_BackSpace:
                if (!screen.search.query.empty()) {
                    screen.search.query.pop_back();
                    screen.find_matches(screen.search.query, screen.search.case_sensitive);
                    m_needs_render = true;
                }
                return;
            default:
                // Let Ctrl-modified keys (Ctrl-Shift-C copy, Ctrl-Shift-V
                // paste, font resize, etc.) fall through to global shortcuts.
                if (ctrl) break;
                if (!key.text.empty() && key.text[0] >= ' ') {
                    screen.search.query += key.text;
                    screen.find_matches(screen.search.query, screen.search.case_sensitive);
                    m_needs_render = true;
                }
                return;
        }
    }

    // Escape when search is active but unfocused: close search
    if (key.keysym == XKB_KEY_Escape && screen.search.active) {
        screen.search.clear();
        m_needs_render = true;
        return;
    }

    // Internal shortcuts
    if (ctrl && shift) {
        switch (key.keysym) {
            case XKB_KEY_N:
            case XKB_KEY_n:
                if (on_new_window) on_new_window();
                return;
            case XKB_KEY_F:
            case XKB_KEY_f:
                if (screen.search.active && !screen.search.focused) {
                    screen.search.focused = true;
                } else {
                    screen.search.active = true;
                    screen.search.focused = true;
                    screen.search.query.clear();
                    screen.search.matches.clear();
                    screen.search.current_match = -1;
                }
                m_needs_render = true;
                return;
            case XKB_KEY_V:
            case XKB_KEY_v:
                paste_into(pane, false);
                return;
            case XKB_KEY_C:
            case XKB_KEY_c: {
                std::string text = screen.get_selection_text();
                if (!text.empty()) {
                    m_platform->set_clipboard(text, false);
                }
                return;
            }
            case XKB_KEY_plus:
            case XKB_KEY_equal:
                m_config.font_size += 1.0f;
                resize_font();
                return;
            case XKB_KEY_minus:
                if (m_config.font_size > 6.0f) {
                    m_config.font_size -= 1.0f;
                    resize_font();
                }
                return;
            case XKB_KEY_0:
                m_config.font_size = kDefaultFontSize;
                resize_font();
                return;
            // Pane splits
            case XKB_KEY_D:
            case XKB_KEY_d:
                if (m_tmux_controller && m_tmux_controller->is_active())
                    m_tmux_client->send_command("split-window -h");
                else if (m_remote_controller && m_remote_controller->is_active())
                    m_remote_controller->split(false);
                else
                    m_tabs->split_pane(SplitDir::Vertical);
                m_needs_render = true;
                return;
            case XKB_KEY_E:
            case XKB_KEY_e:
                if (m_tmux_controller && m_tmux_controller->is_active())
                    m_tmux_client->send_command("split-window -v");
                else if (m_remote_controller && m_remote_controller->is_active())
                    m_remote_controller->split(true);
                else
                    m_tabs->split_pane(SplitDir::Horizontal);
                m_needs_render = true;
                return;
            case XKB_KEY_W:
            case XKB_KEY_w:
                close_focused_pane_routed("last pane closed by key");
                return;
            // Pane navigation
            case XKB_KEY_Left:
                m_tabs->navigate_pane(NavDir::Left);
                return;
            case XKB_KEY_Right:
                m_tabs->navigate_pane(NavDir::Right);
                return;
            case XKB_KEY_Up:
                m_tabs->navigate_pane(NavDir::Up);
                return;
            case XKB_KEY_Down:
                m_tabs->navigate_pane(NavDir::Down);
                return;
            // New tab
            case XKB_KEY_T:
            case XKB_KEY_t:
                if (m_tmux_controller && m_tmux_controller->is_active())
                    m_tmux_client->send_command("new-window");
                else if (m_remote_controller && m_remote_controller->is_active())
                    m_remote_controller->new_window();
                else
                    m_tabs->new_tab();
                recompute();
                m_needs_render = true;
                return;
        }
    }

    // Font size: Ctrl+plus/minus/0 (without Shift)
    if (ctrl && !shift) {
        switch (key.keysym) {
            case XKB_KEY_plus:
            case XKB_KEY_equal:
                m_config.font_size += 1.0f;
                resize_font();
                return;
            case XKB_KEY_minus:
                if (m_config.font_size > 6.0f) {
                    m_config.font_size -= 1.0f;
                    resize_font();
                }
                return;
            case XKB_KEY_0:
                m_config.font_size = kDefaultFontSize;
                resize_font();
                return;
        }
    }

    // Tab cycling: Ctrl+Tab / Ctrl+Shift+Tab
    if (ctrl && !shift && key.keysym == XKB_KEY_Tab) {
        m_tabs->next_tab();
        recompute();
        m_needs_render = true;
        return;
    }
    if (ctrl && shift && key.keysym == XKB_KEY_ISO_Left_Tab) {
        m_tabs->prev_tab();
        recompute();
        m_needs_render = true;
        return;
    }

    // Alt+1..9 (Linux) or Cmd+1..9 (macOS): switch to tab by index.
    // On macOS, Option is a character-composition key (AltGr-style:
    // Option+4 is $ on a Swedish layout), so it must never double as a
    // tab shortcut there.
    bool tab_index_combo;
#ifdef __APPLE__
    tab_index_combo = macos_cmd_held;
#else
    tab_index_combo = key.mods & KeyMod::Alt;
#endif
    if (tab_index_combo && key.keysym >= XKB_KEY_1 && key.keysym <= XKB_KEY_9) {
        int idx = key.keysym - XKB_KEY_1;
        if (idx < m_tabs->tab_count()) {
            m_tabs->activate_tab(idx);
            recompute();
            m_needs_render = true;
        }
        return;
    }

    // Shift+PageUp/Down for scrolling
    if (shift && key.keysym == XKB_KEY_Page_Up) {
        screen.scroll_viewport(-screen.rows() / 2);
        m_needs_render = true;
        return;
    }
    if (shift && key.keysym == XKB_KEY_Page_Down) {
        screen.scroll_viewport(screen.rows() / 2);
        m_needs_render = true;
        return;
    }

    // Don't leak unmatched Cmd-<x> combos into the shell as Ctrl-codes.
    // (Without this guard, Cmd-A would normalize to Ctrl-Shift-A and
    // encode_key would emit ^A.)
    if (macos_cmd_held) return;

    // Forward to PTY
    std::string seq = encode_key(key, screen);
    if (!seq.empty()) {
        if (!screen.at_bottom()) {
            screen.scroll_to_bottom();
            m_needs_render = true;
        }
        pane->write(seq);
    }
}

void Window::handle_mouse(const MouseEvent &mouse) {
    Tab *tab = m_tabs->active_tab();
    if (!tab) return;

    const auto &met = m_renderer.metrics();
    int bar_h = tab_bar_height();

    // Tab bar click and hover handling
    if (bar_h > 0 && mouse.y < bar_h) {
        // Track close button hover
        int close_hover = m_renderer.tab_close_hit_test(*m_tabs, mouse.x, mouse.y, bar_h);
        if (close_hover != m_hover_close_tab) {
            m_hover_close_tab = close_hover;
            m_needs_render = true;
        }

        if (mouse.pressed && !mouse.motion) {
            // Check close button first
            int close_hit = m_renderer.tab_close_hit_test(*m_tabs, mouse.x, mouse.y, bar_h);
            if (close_hit >= 0 && mouse.button == MouseButton::Left) {
                if (m_remote_controller && m_remote_controller->is_active() &&
                    m_remote_controller->request_close_tab(m_tabs->tabs()[close_hit].get())) {
                    // Teardown happens when WindowClosed arrives.
                } else if (!m_tabs->close_tab(close_hit)) {
                    request_close("last tab closed by mouse");
                }
                m_hover_close_tab = -1;
                recompute();
                m_needs_render = true;
                return;
            }

            int hit = m_renderer.tab_hit_test(*m_tabs, mouse.x);
            if (hit >= 0) {
                if (mouse.button == MouseButton::Left) {
                    m_tabs->activate_tab(hit);
                } else if (mouse.button == MouseButton::Middle) {
                    if (m_remote_controller && m_remote_controller->is_active() &&
                        m_remote_controller->request_close_tab(m_tabs->tabs()[hit].get())) {
                        // Teardown happens when WindowClosed arrives.
                    } else if (!m_tabs->close_tab(hit)) {
                        request_close("last tab closed by mouse");
                    }
                }
                recompute();
                m_needs_render = true;
            }
        }
        return;
    }

    // Clear close button hover when mouse leaves tab bar
    if (m_hover_close_tab != -1) {
        m_hover_close_tab = -1;
        m_needs_render = true;
    }

    // Pane divider drag in progress: consume everything until release
    if (m_divider_drag || m_edge_pane) {
        if (mouse.motion) {
            if (m_divider_drag) {
                LayoutTree::drag_divider(m_divider_drag, mouse.x, mouse.y);
                recompute();
                m_needs_render = true;
            } else {
                int cell = m_edge_horizontal ? met.cell_width : met.cell_height;
                int pos = m_edge_horizontal ? mouse.x : mouse.y;
                int cells = cell > 0 ? (pos - m_edge_anchor) / cell : 0;
                if (cells != m_edge_sent && m_remote_controller)
                    m_remote_controller->resize_pane_edge(
                        m_edge_pane, m_edge_horizontal, cells - m_edge_sent);
                m_edge_sent = cells;
            }
        } else if (!mouse.pressed) {
            m_divider_drag = nullptr;
            m_edge_pane = nullptr;
        }
        return;
    }

    // Divider hover / grab: local tabs hit the layout tree; rivtd tabs
    // hit the gaps between pane rects (the daemon owns that layout, so a
    // drag there sends cell deltas instead of moving anything locally).
    if (mouse.y >= bar_h) {
        LayoutNode *div = nullptr;
        Pane *edge_pane = nullptr;
        bool edge_h = false;
        if (!tab->tmux_managed) {
            div = tab->layout.divider_at(mouse.x, mouse.y, 4);
        } else if (m_remote_controller && m_remote_controller->is_active()) {
            for (auto &p : tab->panes) {
                const auto &r = p->rect;
                if (mouse.y >= r.y && mouse.y < r.y + r.h &&
                    mouse.x >= r.x + r.w && mouse.x < r.x + r.w + met.cell_width &&
                    r.x + r.w + met.cell_width < m_win_w) {
                    edge_pane = p.get(); edge_h = true; break;
                }
                if (mouse.x >= r.x && mouse.x < r.x + r.w &&
                    mouse.y >= r.y + r.h && mouse.y < r.y + r.h + met.cell_height &&
                    r.y + r.h + met.cell_height < m_win_h) {
                    edge_pane = p.get(); edge_h = false; break;
                }
            }
        }
        if (div || edge_pane) {
            bool horiz = div ? (div->split_dir == SplitDir::Vertical) : edge_h;
            m_platform->set_mouse_cursor(horiz ? Platform::MouseCursor::ResizeH
                                               : Platform::MouseCursor::ResizeV);
            m_resize_cursor = true;
            if (mouse.pressed && !mouse.motion && mouse.button == MouseButton::Left) {
                if (div) {
                    m_divider_drag = div;
                } else {
                    m_edge_pane = edge_pane;
                    m_edge_horizontal = edge_h;
                    m_edge_anchor = edge_h ? mouse.x : mouse.y;
                    m_edge_sent = 0;
                }
            }
            return;
        }
        if (m_resize_cursor) {
            m_resize_cursor = false;
            m_platform->set_mouse_cursor(Platform::MouseCursor::Default);
        }
    }

    // Route mouse to correct pane
    Pane *target_pane = nullptr;
    if (tab->tmux_managed) {
        for (auto &p : tab->panes) {
            auto &r = p->rect;
            if (mouse.x >= r.x && mouse.x < r.x + r.w &&
                mouse.y >= r.y && mouse.y < r.y + r.h) {
                target_pane = p.get();
                break;
            }
        }
    } else {
        target_pane = tab->layout.pane_at(mouse.x, mouse.y);
    }
    if (!target_pane) {
        // Mouse is outside all panes (e.g. release after dragging out of window).
        // Deliver to the focused pane so selection drag can finish.
        target_pane = tab->focused_pane;
        if (!target_pane) return;
    }

    // Focus follows mouse
    if (target_pane != tab->focused_pane) {
        tab->focused_pane = target_pane;
        target_pane->has_activity = false;
        m_needs_render = true;
    }

    ScreenBuffer &screen = target_pane->screen();
    const auto &pr = target_pane->rect;

    // Compute cell coords relative to pane
    int local_x = mouse.x - pr.x;
    int local_y = mouse.y - pr.y;
    int cell_col = met.cell_width > 0 ? local_x / met.cell_width : 0;
    int cell_row = met.cell_height > 0 ? local_y / met.cell_height : 0;
    int cols = screen.cols();
    int rows = screen.rows();

    if (cell_col < 0) cell_col = 0;
    if (cell_row < 0) cell_row = 0;
    if (cell_col >= cols) cell_col = cols - 1;
    if (cell_row >= rows) cell_row = rows - 1;

    int mm = screen.mouse_mode();
    if (mm) {
        bool report = false;
        if (mouse.motion) {
            if (mm == 1003) report = true;
            else if (mm == 1002 && mouse.button != MouseButton::NoButton) report = true;
        } else {
            report = true;
        }

        if (report) {
            std::string seq = encode_mouse(mouse, cell_col, cell_row,
                                           screen.sgr_mouse());
            if (!seq.empty()) {
                target_pane->write(seq);
                return;
            }
        }
    }

    // Fallback scroll. scroll_lines > 0 is an exact count from a
    // precise device (trackpad); a discrete wheel click is worth 3.
    if (mouse.button == MouseButton::ScrollUp || mouse.button == MouseButton::ScrollDown) {
        int n = mouse.scroll_lines > 0 ? mouse.scroll_lines : 3;
        if (screen.alt_screen()) {
            const char *arrow = mouse.button == MouseButton::ScrollUp
                ? (screen.app_cursor_keys() ? "\033OA" : "\033[A")
                : (screen.app_cursor_keys() ? "\033OB" : "\033[B");
            for (int i = 0; i < n; i++)
                target_pane->write(std::string(arrow));
        } else {
            int delta = mouse.button == MouseButton::ScrollUp ? -n : n;
            screen.scroll_viewport(delta);
            m_needs_render = true;
        }
    }

    // URL detection: cursor shape on modifier+hover, open on
    // modifier+click. Cmd on macOS (the platform convention), Ctrl
    // elsewhere.
#ifdef __APPLE__
    bool url_mod = mouse.mods & KeyMod::Super;
    static constexpr const char *kOpener = "open";
#else
    bool url_mod = mouse.mods & KeyMod::Ctrl;
    static constexpr const char *kOpener = "xdg-open";
#endif
    if (url_mod && m_config.url_detection) {
        std::string url = screen.detect_url_at(cell_row, cell_col);
        if (!url.empty()) {
            m_platform->set_mouse_cursor(Platform::MouseCursor::Hand);
            if (mouse.button == MouseButton::Left && mouse.pressed && !mouse.motion) {
                // Open the URL, fire and forget
                pid_t pid;
                const char *argv[] = {kOpener, url.c_str(), nullptr};
                posix_spawnp(&pid, kOpener, nullptr, nullptr,
                             const_cast<char **>(argv), ::environ);
                return;
            }
        } else {
            m_platform->set_mouse_cursor(Platform::MouseCursor::Default);
        }
    } else if (!url_mod) {
        m_platform->set_mouse_cursor(Platform::MouseCursor::Default);
    }

    // Selection handling (only when mouse mode is off)
    if (!screen.mouse_mode()) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (mouse.button == MouseButton::Left && mouse.pressed && !mouse.motion) {
            int abs_line = screen.absolute_line(cell_row);

            if (now_ms - target_pane->last_click_ms < 400 &&
                cell_col == target_pane->last_click_col && cell_row == target_pane->last_click_row) {
                target_pane->click_count = (target_pane->click_count % 4) + 1;
            } else {
                target_pane->click_count = 1;
            }
            target_pane->last_click_ms = now_ms;
            target_pane->last_click_col = cell_col;
            target_pane->last_click_row = cell_row;

            if (target_pane->click_count == 1) {
                target_pane->selecting = true;
                screen.selection.active = true;
                screen.selection.rectangular = (mouse.mods & KeyMod::Shift);
                screen.selection.start_line = abs_line;
                screen.selection.start_col = cell_col;
                screen.selection.end_line = abs_line;
                screen.selection.end_col = cell_col;
            } else if (target_pane->click_count == 2) {
                target_pane->selecting = false;
                const Line &line = screen.line(cell_row);
                int wstart = cell_col, wend = cell_col;
                auto is_word_char = [](uint32_t cp) {
                    return cp > ' ' && cp != '"' && cp != '\'' &&
                           cp != '(' && cp != ')' && cp != '[' && cp != ']' &&
                           cp != '{' && cp != '}' && cp != '<' && cp != '>';
                };
                while (wstart > 0 && wstart - 1 < (int)line.cells.size() &&
                       is_word_char(line.cells[wstart - 1].codepoint))
                    wstart--;
                while (wend + 1 < (int)line.cells.size() &&
                       is_word_char(line.cells[wend + 1].codepoint))
                    wend++;
                screen.selection.active = true;
                screen.selection.start_line = abs_line;
                screen.selection.start_col = wstart;
                screen.selection.end_line = abs_line;
                screen.selection.end_col = wend;
                std::string text = screen.get_selection_text();
                if (!text.empty()) m_platform->set_clipboard(text, true);
            } else if (target_pane->click_count == 3) {
                target_pane->selecting = false;
                const Line &line = screen.line(cell_row);
                screen.selection.active = true;
                screen.selection.start_line = abs_line;
                screen.selection.start_col = 0;
                screen.selection.end_line = abs_line;
                screen.selection.end_col = (int)line.cells.size() - 1;
                std::string text = screen.get_selection_text();
                if (!text.empty()) m_platform->set_clipboard(text, true);
            } else if (target_pane->click_count == 4) {
                // Quadruple-click: select entire command output (OSC 133 zones)
                target_pane->selecting = false;
                int out_start, out_end;
                if (screen.find_command_output(abs_line, out_start, out_end)) {
                    screen.selection.active = true;
                    screen.selection.start_line = out_start;
                    screen.selection.start_col = 0;
                    screen.selection.end_line = out_end;
                    screen.selection.end_col = screen.cols() - 1;
                    std::string text = screen.get_selection_text();
                    if (!text.empty()) m_platform->set_clipboard(text, true);
                }
            }
            m_needs_render = true;
        } else if (mouse.motion && target_pane->selecting) {
            screen.selection.end_line = screen.absolute_line(cell_row);
            screen.selection.end_col = cell_col;
            m_needs_render = true;
        } else if (mouse.button == MouseButton::Left && !mouse.pressed && !mouse.motion) {
            if (target_pane->selecting) {
                target_pane->selecting = false;
                int sl, sc, el, ec;
                screen.selection.normalized(sl, sc, el, ec);
                if (sl == el && sc == ec) {
                    screen.selection.clear();
                } else {
                    std::string text = screen.get_selection_text();
                    if (!text.empty()) {
                        m_platform->set_clipboard(text, true);
                    }
                }
                m_needs_render = true;
            }
        } else if (mouse.button == MouseButton::Right && mouse.pressed && !mouse.motion) {
            int abs_line = screen.absolute_line(cell_row);
            if (!screen.selection.active) {
                // First right-click: set anchor point
                screen.selection.active = true;
                screen.selection.rectangular = false;
                screen.selection.start_line = abs_line;
                screen.selection.start_col = cell_col;
                screen.selection.end_line = abs_line;
                screen.selection.end_col = cell_col;
            } else {
                // Move whichever endpoint is closest to the click
                int sl, sc, el, ec;
                screen.selection.normalized(sl, sc, el, ec);

                // Distance from click to start vs end (line distance dominates)
                int dist_start = std::abs(abs_line - sl) * screen.cols()
                               + std::abs(cell_col - sc);
                int dist_end = std::abs(abs_line - el) * screen.cols()
                             + std::abs(cell_col - ec);

                if (dist_start <= dist_end) {
                    screen.selection.start_line = abs_line;
                    screen.selection.start_col = cell_col;
                } else {
                    screen.selection.end_line = abs_line;
                    screen.selection.end_col = cell_col;
                }

                // Copy to primary clipboard if non-degenerate
                std::string text = screen.get_selection_text();
                if (!text.empty())
                    m_platform->set_clipboard(text, true);
            }
            m_needs_render = true;
        } else if (mouse.button == MouseButton::Middle && mouse.pressed) {
            paste_into(target_pane, true);
        }
    }
}

void Window::render_if_needed() {
    // Clean up deferred tmux objects (safe now — call stack has unwound)
    m_tmux_stale_controller.reset();
    m_tmux_stale_client.reset();

    if (!needs_render()) return;

    auto rs_t0 = std::chrono::steady_clock::now();

    m_platform->make_current();
    m_renderer.begin_frame(m_config);

    Tab *tab = m_tabs->active_tab();
    if (tab) {
        int bar_h = tab_bar_height();

        // Render tab bar if multiple tabs
        if (bar_h > 0) {
            m_renderer.render_tab_bar(*m_tabs, m_config, bar_h, m_hover_close_tab);
            m_renderer.flush();
        }

        // Render dot grid in dead zone for tmux-managed tabs
        if (tab->tmux_managed) {
            const auto &m = m_renderer.metrics();
            m_renderer.render_dot_grid(0, bar_h, m_win_w, m_win_h - bar_h,
                                       m.cell_width, m.cell_height);
            m_renderer.flush();
        }

        // Render each pane with scissor clipping
        std::vector<Pane *> panes;
        if (tab->tmux_managed) {
            for (auto &p : tab->panes) panes.push_back(p.get());
        } else {
            tab->layout.collect_panes(panes);
        }

        for (Pane *p : panes) {
            const auto &r = p->rect;
            m_renderer.render_pane(p->screen(), m_config, r.x, r.y, r.w, r.h,
                                 p == tab->focused_pane);
        }

        // Render borders between panes (over the top, no scissor)
        if (panes.size() > 1) {
            std::function<void(const LayoutNode *)> draw_borders;
            draw_borders = [&](const LayoutNode *node) {
                if (!node || node->is_leaf()) return;

                if (node->split_dir == SplitDir::Vertical) {
                    std::vector<Pane *> fp, sp;
                    auto collect = [](const LayoutNode *n, std::vector<Pane *> &out, auto &self) -> void {
                        if (n->is_leaf()) { out.push_back(n->pane); return; }
                        self(n->first.get(), out, self);
                        self(n->second.get(), out, self);
                    };
                    collect(node->first.get(), fp, collect);
                    collect(node->second.get(), sp, collect);

                    if (!fp.empty() && !sp.empty()) {
                        int right_edge = 0;
                        int top_edge = INT_MAX;
                        int bottom_edge = 0;
                        for (auto *p : fp) {
                            right_edge = std::max(right_edge, p->rect.x + p->rect.w);
                            top_edge = std::min(top_edge, p->rect.y);
                            bottom_edge = std::max(bottom_edge, p->rect.y + p->rect.h);
                        }
                        int left_edge = INT_MAX;
                        for (auto *p : sp) {
                            left_edge = std::min(left_edge, p->rect.x);
                        }
                        int border_x = right_edge;
                        int border_w = left_edge - right_edge;
                        if (border_w > 0) {
                            m_renderer.render_border((float)border_x, (float)top_edge,
                                                   (float)border_w, (float)(bottom_edge - top_edge),
                                                   0.3f, 0.3f, 0.3f);
                        }
                    }
                } else {
                    // Horizontal border
                    std::vector<Pane *> fp, sp;
                    auto collect = [](const LayoutNode *n, std::vector<Pane *> &out, auto &self) -> void {
                        if (n->is_leaf()) { out.push_back(n->pane); return; }
                        self(n->first.get(), out, self);
                        self(n->second.get(), out, self);
                    };
                    collect(node->first.get(), fp, collect);
                    collect(node->second.get(), sp, collect);

                    if (!fp.empty() && !sp.empty()) {
                        int bottom_edge = 0;
                        int left_edge = INT_MAX;
                        int right_edge = 0;
                        for (auto *p : fp) {
                            bottom_edge = std::max(bottom_edge, p->rect.y + p->rect.h);
                            left_edge = std::min(left_edge, p->rect.x);
                            right_edge = std::max(right_edge, p->rect.x + p->rect.w);
                        }
                        int top_edge_s = INT_MAX;
                        for (auto *p : sp) {
                            top_edge_s = std::min(top_edge_s, p->rect.y);
                        }
                        int border_y = bottom_edge;
                        int border_h = top_edge_s - bottom_edge;
                        if (border_h > 0) {
                            m_renderer.render_border((float)left_edge, (float)border_y,
                                                   (float)(right_edge - left_edge), (float)border_h,
                                                   0.3f, 0.3f, 0.3f);
                        }
                    }
                }

                draw_borders(node->first.get());
                draw_borders(node->second.get());
            };

            draw_borders(tab->layout.root());
            m_renderer.flush();
        }
    }

    auto rs_t1 = std::chrono::steady_clock::now();
    m_platform->swap_buffers();
    m_needs_render = false;
    m_last_frame = std::chrono::steady_clock::now();

    // --debug: per-second render summary. Separates the three lag
    // signatures: a render loop that never goes idle (frames/s pegged at
    // the refresh rate while nothing is happening), a swap_buffers that
    // starts blocking beyond one vsync (driver/compositor), and frame
    // builds that got expensive (e.g. glyph atlas being rebuilt).
    if (debug_enabled()) {
        auto rs_t2 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        double build = ms(rs_t0, rs_t1), swap = ms(rs_t1, rs_t2);
        m_rs_frames++;
        m_rs_build_sum += build;
        m_rs_swap_sum += swap;
        m_rs_build_max = std::max(m_rs_build_max, build);
        m_rs_swap_max = std::max(m_rs_swap_max, swap);
        double since_log = ms(m_rs_last_log, rs_t2);
        if (m_rs_last_log.time_since_epoch().count() == 0) {
            m_rs_last_log = rs_t2;
        } else if (since_log >= 1000.0) {
            dbg("window(%p): render %.1f fps, build avg %.2f max %.2f ms, "
                "swap avg %.2f max %.2f ms",
                (void *)this, m_rs_frames * 1000.0 / since_log,
                m_rs_build_sum / m_rs_frames, m_rs_build_max,
                m_rs_swap_sum / m_rs_frames, m_rs_swap_max);
            m_rs_frames = 0;
            m_rs_build_sum = m_rs_swap_sum = 0;
            m_rs_build_max = m_rs_swap_max = 0;
            m_rs_last_log = rs_t2;
        }
    }
}

} // namespace rivt
