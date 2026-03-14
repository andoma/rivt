#include "core/window.h"
#include "core/event_loop.h"
#include "core/debug.h"
#include "tmux/tmux_client.h"
#include "tmux/tmux_controller.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <climits>
#include <chrono>
#include <cstdio>

namespace rivt {

// Extra pixels below the last row so descenders/underscores aren't clipped
static constexpr int kBottomPad = 2;

Window::Window(const Config &base_config, EventLoop &loop)
    : m_config(base_config), m_loop(loop) {}

Window::~Window() {
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
        fprintf(stderr, "Failed to create platform\n");
        return false;
    }

    if (!m_platform->create_window(m_win_w, m_win_h, "rivt")) {
        fprintf(stderr, "Failed to create window\n");
        return false;
    }

    if (!m_platform->create_gl_context()) {
        fprintf(stderr, "Failed to create GL context\n");
        return false;
    }

    if (!m_renderer.init(m_config)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return false;
    }

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        if (on_close) on_close(this);
    };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height + kBottomPad;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);

    if (!m_tabs->new_tab()) {
        fprintf(stderr, "Failed to spawn initial shell\n");
        return false;
    }
    recompute();

    m_platform->show_window();
    setup_callbacks();
    return true;
}

bool Window::init_tmux(const std::vector<std::string> &tmux_args) {
    m_platform = Platform::create();
    if (!m_platform) {
        fprintf(stderr, "Failed to create platform\n");
        return false;
    }

    if (!m_platform->create_window(m_win_w, m_win_h, "rivt [tmux]")) {
        fprintf(stderr, "Failed to create window\n");
        return false;
    }

    if (!m_platform->create_gl_context()) {
        fprintf(stderr, "Failed to create GL context\n");
        return false;
    }

    if (!m_renderer.init(m_config)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return false;
    }

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        if (on_close) on_close(this);
    };

    const auto &m = m_renderer.metrics();
    m_tabs->set_cell_size(m.cell_width, m.cell_height);
    m_win_w = m_config.initial_cols * m.cell_width;
    m_win_h = m_config.initial_rows * m.cell_height + kBottomPad;
    m_platform->resize_window(m_win_w, m_win_h);
    m_renderer.set_viewport(m_win_w, m_win_h);

    // Create tmux client and controller (no initial tab — tmux notifications create them)
    m_tmux_client = std::make_unique<TmuxClient>(m_loop);
    m_tmux_controller = std::make_unique<TmuxController>(*m_tmux_client, *this, *m_tabs, m_loop);

    if (!m_tmux_client->start(tmux_args)) {
        fprintf(stderr, "Failed to start tmux -CC\n");
        return false;
    }

    int bar_h = tab_bar_height();
    int cols = m.cell_width > 0 ? m_win_w / m.cell_width : 80;
    int rows = m.cell_height > 0 ? (m_win_h - bar_h - kBottomPad) / m.cell_height : 24;
    m_tmux_controller->initialize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);

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
        m_closing = true;
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
    if (!m_renderer.init(m_config)) return false;

    m_renderer.set_viewport(m_win_w, m_win_h);

    m_tabs = std::make_unique<TabManager>(m_config, m_loop, m_platform.get());
    m_tabs->on_needs_render = [this]() { m_needs_render = true; };
    m_tabs->on_quit = [this]() {
        if (on_close) on_close(this);
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
        gateway_pane->pty().write(data);
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

void Window::handle_resize(int w, int h) {
    dbg("window(%p): handle_resize %dx%d tmux=%d gateway=%p",
        (void*)this, w, h,
        m_tmux_controller && m_tmux_controller->is_active(),
        (void*)m_tmux_gateway_pane);
    m_win_w = w;
    m_win_h = h;
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

    recompute();
    m_needs_render = true;
}

void Window::handle_key(const KeyEvent &key) {
    if (!key.pressed) return;

    Pane *pane = m_tabs->focused_pane();
    if (!pane) return;

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
                    int delta = shift ? -1 : 1;
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
            case XKB_KEY_v: {
                std::string text = m_platform->get_clipboard(false);
                if (!text.empty()) {
                    if (screen.bracketed_paste()) {
                        pane->write("\033[200~");
                        pane->write(text);
                        pane->write("\033[201~");
                    } else {
                        pane->write(text);
                    }
                }
                return;
            }
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
                m_config.font_size = 11.0f;
                resize_font();
                return;
            // Pane splits
            case XKB_KEY_D:
            case XKB_KEY_d:
                if (m_tmux_controller && m_tmux_controller->is_active())
                    m_tmux_client->send_command("split-window -h");
                else
                    m_tabs->split_pane(SplitDir::Vertical);
                m_needs_render = true;
                return;
            case XKB_KEY_E:
            case XKB_KEY_e:
                if (m_tmux_controller && m_tmux_controller->is_active())
                    m_tmux_client->send_command("split-window -v");
                else
                    m_tabs->split_pane(SplitDir::Horizontal);
                m_needs_render = true;
                return;
            case XKB_KEY_W:
            case XKB_KEY_w:
                if (m_tmux_controller && m_tmux_controller->is_active()) {
                    m_tmux_client->send_command("kill-pane");
                } else {
                    if (!m_tabs->close_focused_pane()) {
                        if (on_close) on_close(this);
                    }
                }
                m_needs_render = true;
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
                m_config.font_size = 11.0f;
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

    // Alt+1..9: switch to tab by index
    bool alt = key.mods & KeyMod::Alt;
    if (alt && key.keysym >= XKB_KEY_1 && key.keysym <= XKB_KEY_9) {
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

    // Tab bar click handling
    if (bar_h > 0 && mouse.y < bar_h) {
        if (mouse.pressed && !mouse.motion) {
            int hit = m_renderer.tab_hit_test(*m_tabs, mouse.x);
            if (hit >= 0) {
                if (mouse.button == MouseButton::Left) {
                    m_tabs->activate_tab(hit);
                } else if (mouse.button == MouseButton::Middle) {
                    if (!m_tabs->close_tab(hit)) {
                        if (on_close) on_close(this);
                    }
                }
                recompute();
                m_needs_render = true;
            }
        }
        return;
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

    // Fallback scroll
    if (mouse.button == MouseButton::ScrollUp || mouse.button == MouseButton::ScrollDown) {
        if (screen.alt_screen()) {
            const char *arrow = mouse.button == MouseButton::ScrollUp
                ? (screen.app_cursor_keys() ? "\033OA" : "\033[A")
                : (screen.app_cursor_keys() ? "\033OB" : "\033[B");
            for (int i = 0; i < 3; i++)
                target_pane->write(std::string(arrow));
        } else {
            int delta = mouse.button == MouseButton::ScrollUp ? -3 : 3;
            screen.scroll_viewport(delta);
            m_needs_render = true;
        }
    }

    // Selection handling (only when mouse mode is off)
    if (!screen.mouse_mode()) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (mouse.button == MouseButton::Left && mouse.pressed && !mouse.motion) {
            int abs_line = screen.absolute_line(cell_row);

            if (now_ms - target_pane->last_click_ms < 400 &&
                cell_col == target_pane->last_click_col && cell_row == target_pane->last_click_row) {
                target_pane->click_count = (target_pane->click_count % 3) + 1;
            } else {
                target_pane->click_count = 1;
            }
            target_pane->last_click_ms = now_ms;
            target_pane->last_click_col = cell_col;
            target_pane->last_click_row = cell_row;

            if (target_pane->click_count == 1) {
                target_pane->selecting = true;
                screen.selection.active = true;
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
        } else if (mouse.button == MouseButton::Middle && mouse.pressed) {
            std::string text = m_platform->get_clipboard(true);
            if (!text.empty()) {
                if (screen.bracketed_paste()) {
                    target_pane->write("\033[200~");
                    target_pane->write(text);
                    target_pane->write("\033[201~");
                } else {
                    target_pane->write(text);
                }
            }
        }
    }
}

void Window::render_if_needed() {
    // Clean up deferred tmux objects (safe now — call stack has unwound)
    m_tmux_stale_controller.reset();
    m_tmux_stale_client.reset();

    if (!m_needs_render) return;

    m_platform->make_current();
    m_renderer.begin_frame(m_config);

    Tab *tab = m_tabs->active_tab();
    if (tab) {
        int bar_h = tab_bar_height();

        // Render tab bar if multiple tabs
        if (bar_h > 0) {
            m_renderer.render_tab_bar(*m_tabs, m_config, bar_h);
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

    m_platform->swap_buffers();
    m_needs_render = false;
}

} // namespace rivt
