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

Window::Window(const Config &base_config, EventLoop &loop)
    : config_(base_config), loop_(loop) {}

Window::~Window() {
    // Replace the gateway pane's override with a drain that swallows remaining
    // tmux protocol (up to the DCS terminator \033\\) before restoring normal
    // operation. Don't touch on_tmux_control_mode — it stays active so the
    // user can run tmux -CC again.
    if (tmux_gateway_pane_) {
        Pane *gw = tmux_gateway_pane_;
        gw->pty_data_override_ = [gw](const char *buf, int len) {
            // Swallow tmux protocol. Look for ST (\033\\) which terminates
            // the DCS that started CC mode — everything after it is normal.
            std::string_view sv(buf, len);
            auto pos = sv.find("\033\\");
            if (pos != std::string_view::npos) {
                gw->pty_data_override_ = nullptr;
                // Feed any data after ST to the pane normally
                const char *after = buf + pos + 2;
                int remaining = len - (int)(pos + 2);
                if (remaining > 0) {
                    gw->feed_data(after, remaining);
                }
            }
        };
        tmux_gateway_pane_ = nullptr;
    }
}

bool Window::init() {
    platform_ = Platform::create();
    if (!platform_) {
        fprintf(stderr, "Failed to create platform\n");
        return false;
    }

    if (!platform_->create_window(win_w_, win_h_, "rivt")) {
        fprintf(stderr, "Failed to create window\n");
        return false;
    }

    if (!platform_->create_gl_context()) {
        fprintf(stderr, "Failed to create GL context\n");
        return false;
    }

    if (!renderer_.init(config_)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return false;
    }

    renderer_.set_viewport(win_w_, win_h_);

    tabs_ = std::make_unique<TabManager>(config_, loop_, platform_.get());
    tabs_->on_needs_render = [this]() { needs_render_ = true; };
    tabs_->on_quit = [this]() {
        if (on_close) on_close(this);
    };

    const auto &m = renderer_.metrics();
    tabs_->set_cell_size(m.cell_width, m.cell_height);
    win_w_ = config_.initial_cols * m.cell_width;
    win_h_ = config_.initial_rows * m.cell_height;
    platform_->resize_window(win_w_, win_h_);
    renderer_.set_viewport(win_w_, win_h_);

    if (!tabs_->new_tab()) {
        fprintf(stderr, "Failed to spawn initial shell\n");
        return false;
    }
    recompute();

    platform_->show_window();
    setup_callbacks();
    return true;
}

bool Window::init_tmux(const std::vector<std::string> &tmux_args) {
    platform_ = Platform::create();
    if (!platform_) {
        fprintf(stderr, "Failed to create platform\n");
        return false;
    }

    if (!platform_->create_window(win_w_, win_h_, "rivt [tmux]")) {
        fprintf(stderr, "Failed to create window\n");
        return false;
    }

    if (!platform_->create_gl_context()) {
        fprintf(stderr, "Failed to create GL context\n");
        return false;
    }

    if (!renderer_.init(config_)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return false;
    }

    renderer_.set_viewport(win_w_, win_h_);

    tabs_ = std::make_unique<TabManager>(config_, loop_, platform_.get());
    tabs_->on_needs_render = [this]() { needs_render_ = true; };
    tabs_->on_quit = [this]() {
        if (on_close) on_close(this);
    };

    const auto &m = renderer_.metrics();
    tabs_->set_cell_size(m.cell_width, m.cell_height);
    win_w_ = config_.initial_cols * m.cell_width;
    win_h_ = config_.initial_rows * m.cell_height;
    platform_->resize_window(win_w_, win_h_);
    renderer_.set_viewport(win_w_, win_h_);

    // Create tmux client and controller (no initial tab — tmux notifications create them)
    tmux_client_ = std::make_unique<TmuxClient>(loop_);
    tmux_controller_ = std::make_unique<TmuxController>(*tmux_client_, *this, *tabs_, loop_);

    if (!tmux_client_->start(tmux_args)) {
        fprintf(stderr, "Failed to start tmux -CC\n");
        return false;
    }

    int bar_h = tab_bar_height();
    int cols = m.cell_width > 0 ? win_w_ / m.cell_width : 80;
    int rows = m.cell_height > 0 ? (win_h_ - bar_h) / m.cell_height : 24;
    tmux_controller_->initialize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);

    platform_->show_window();
    setup_callbacks();
    return true;
}

int Window::tab_bar_height() const {
    const auto &m = renderer_.metrics();
    return tabs_->tab_count() > 1 ? m.cell_height + 8 : 0;
}

void Window::recompute() {
    int bar_h = tab_bar_height();
    tabs_->recompute_layout(0, bar_h, win_w_, win_h_ - bar_h);
    update_size_hints();
}

void Window::update_size_hints() {
    const auto &m = renderer_.metrics();
    if (m.cell_width > 0 && m.cell_height > 0) {
        int bar_h = tab_bar_height();
        platform_->set_size_hints(m.cell_width, m.cell_height, 0, bar_h);
    }
}

void Window::resize_to_cells(int cols, int rows) {
    const auto &m = renderer_.metrics();
    // Always reserve space for the tab bar — tmux sessions typically have
    // multiple windows, and the bar will appear once the second tab arrives.
    int bar_h = m.cell_height + 8;
    win_w_ = cols * m.cell_width;
    win_h_ = rows * m.cell_height + bar_h;
    dbg("window: resize_to_cells %dx%d -> %dx%d px (bar_h=%d cell=%dx%d)",
        cols, rows, win_w_, win_h_, bar_h, m.cell_width, m.cell_height);
    platform_->resize_window(win_w_, win_h_);
    renderer_.set_viewport(win_w_, win_h_);
    recompute();
    needs_render_ = true;
}

void Window::resize_font() {
    int cols, rows;
    renderer_.compute_grid(win_w_, win_h_ - tab_bar_height(), cols, rows);
    renderer_.set_font_size(config_.font_size, platform_->get_dpi_scale() * 96.0f);
    tabs_->set_cell_size(renderer_.metrics().cell_width, renderer_.metrics().cell_height);
    const auto &met = renderer_.metrics();
    win_w_ = cols * met.cell_width;
    win_h_ = rows * met.cell_height + tab_bar_height();
    platform_->resize_window(win_w_, win_h_);
    renderer_.set_viewport(win_w_, win_h_);
    recompute();
    needs_render_ = true;
}

void Window::toggle_cursor_blink() {
    cursor_blink_on_ = !cursor_blink_on_;
    needs_render_ = true;
}

bool Window::reap_dead_panes() {
    if (!tabs_->reap_dead_panes()) {
        return false;
    }
    recompute();
    needs_render_ = true;
    return true;
}

void Window::setup_callbacks() {
    platform_->on_key = [this](const KeyEvent &key) { handle_key(key); };
    platform_->on_mouse = [this](const MouseEvent &mouse) { handle_mouse(mouse); };
    platform_->on_resize = [this](int w, int h) { handle_resize(w, h); };

    platform_->on_focus = [this](bool focused) {
        focused_ = focused;
        renderer_.set_focused(focused);
        needs_render_ = true;
        Pane *pane = tabs_->focused_pane();
        if (pane && pane->screen().focus_reporting()) {
            pane->write(focused ? "\033[I" : "\033[O");
        }
    };

    platform_->on_close = [this]() {
        if (tmux_controller_ && tmux_controller_->is_active()) {
            tmux_controller_->detach();
        }
        closing_ = true;
    };

    // Detect tmux -CC control mode in any pane's PTY output
    tabs_->on_tmux_control_mode = [this](Pane *pane) {
        start_tmux_from_pane(pane);
    };
}

void Window::start_tmux_from_pane(Pane *gateway) {
    dbg("window: start_tmux_from_pane gateway=%p", (void*)gateway);
    if (on_new_tmux_window) on_new_tmux_window(gateway);
}

bool Window::init_tmux_pty(Pane *gateway_pane) {
    dbg("window: init_tmux_pty gateway=%p", (void*)gateway_pane);
    platform_ = Platform::create();
    if (!platform_) return false;
    if (!platform_->create_window(win_w_, win_h_, "rivt [tmux]")) return false;
    if (!platform_->create_gl_context()) return false;
    if (!renderer_.init(config_)) return false;

    renderer_.set_viewport(win_w_, win_h_);

    tabs_ = std::make_unique<TabManager>(config_, loop_, platform_.get());
    tabs_->on_needs_render = [this]() { needs_render_ = true; };
    tabs_->on_quit = [this]() {
        if (on_close) on_close(this);
    };

    const auto &m = renderer_.metrics();
    tabs_->set_cell_size(m.cell_width, m.cell_height);
    win_w_ = config_.initial_cols * m.cell_width;
    win_h_ = config_.initial_rows * m.cell_height;
    platform_->resize_window(win_w_, win_h_);
    renderer_.set_viewport(win_w_, win_h_);

    tmux_gateway_pane_ = gateway_pane;

    // Create tmux client in PTY mode — writes go to the gateway pane's PTY
    tmux_client_ = std::make_unique<TmuxClient>(loop_);
    tmux_client_->start_pty_mode([gateway_pane](const std::string &data) {
        gateway_pane->pty().write(data);
    });

    tmux_controller_ = std::make_unique<TmuxController>(*tmux_client_, *this, *tabs_, loop_);
    tmux_controller_->set_gateway_pane(gateway_pane);
    tmux_controller_->on_tmux_exit = [this]() {
        stop_tmux_pty_mode();
    };

    // Redirect gateway pane's PTY reads to our tmux client
    gateway_pane->pty_data_override_ = [this](const char *buf, int len) {
        tmux_client_->feed_data(buf, len);
    };

    int bar_h = tab_bar_height();
    int cols = m.cell_width > 0 ? win_w_ / m.cell_width : 80;
    int rows = m.cell_height > 0 ? (win_h_ - bar_h) / m.cell_height : 24;
    tmux_controller_->initialize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);

    platform_->show_window();
    setup_callbacks();
    return true;
}

void Window::stop_tmux_pty_mode() {
    if (!tmux_gateway_pane_) return;

    // Restore gateway pane to normal operation
    tmux_gateway_pane_->pty_data_override_ = nullptr;
    tmux_gateway_pane_ = nullptr;

    // Defer destruction — we're likely inside tmux_client_->feed_data() call stack
    tmux_stale_controller_ = std::move(tmux_controller_);
    tmux_stale_client_ = std::move(tmux_client_);

    // Close this tmux window
    closing_ = true;
}

void Window::handle_resize(int w, int h) {
    dbg("window: handle_resize %dx%d", w, h);
    win_w_ = w;
    win_h_ = h;
    renderer_.set_viewport(w, h);
    const auto &m = renderer_.metrics();
    tabs_->set_cell_size(m.cell_width, m.cell_height);

    if (tmux_controller_ && tmux_controller_->is_active()) {
        int bar_h = tab_bar_height();
        int cols = m.cell_width > 0 ? w / m.cell_width : 80;
        int rows = m.cell_height > 0 ? (h - bar_h) / m.cell_height : 24;
        tmux_controller_->handle_resize(cols, rows, m.cell_width, m.cell_height, 0, bar_h);
    }

    recompute();
    needs_render_ = true;
}

void Window::handle_key(const KeyEvent &key) {
    if (!key.pressed) return;

    Pane *pane = tabs_->focused_pane();
    if (!pane) return;

    ScreenBuffer &screen = pane->screen();
    bool ctrl  = key.mods & KeyMod::Ctrl;
    bool shift = key.mods & KeyMod::Shift;

    // Search mode input handling
    if (screen.search.focused) {
        switch (key.keysym) {
            case XKB_KEY_Escape:
                screen.search.focused = false;
                needs_render_ = true;
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
                        int target_offset = -(int)screen.scrollback_count() + match.abs_line - rows / 2;
                        screen.scroll_viewport(target_offset - screen.viewport_offset());
                    }
                    needs_render_ = true;
                }
                return;
            }
            case XKB_KEY_BackSpace:
                if (!screen.search.query.empty()) {
                    screen.search.query.pop_back();
                    screen.find_matches(screen.search.query, screen.search.case_sensitive);
                    needs_render_ = true;
                }
                return;
            default:
                if (!key.text.empty() && key.text[0] >= ' ') {
                    screen.search.query += key.text;
                    screen.find_matches(screen.search.query, screen.search.case_sensitive);
                    needs_render_ = true;
                }
                return;
        }
    }

    // Escape when search is active but unfocused: close search
    if (key.keysym == XKB_KEY_Escape && screen.search.active) {
        screen.search.clear();
        needs_render_ = true;
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
                needs_render_ = true;
                return;
            case XKB_KEY_V:
            case XKB_KEY_v: {
                std::string text = platform_->get_clipboard(false);
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
                    platform_->set_clipboard(text, false);
                }
                return;
            }
            case XKB_KEY_plus:
            case XKB_KEY_equal:
                config_.font_size += 1.0f;
                resize_font();
                return;
            case XKB_KEY_minus:
                if (config_.font_size > 6.0f) {
                    config_.font_size -= 1.0f;
                    resize_font();
                }
                return;
            case XKB_KEY_0:
                config_.font_size = 10.0f;
                resize_font();
                return;
            // Pane splits
            case XKB_KEY_D:
            case XKB_KEY_d:
                if (tmux_controller_ && tmux_controller_->is_active())
                    tmux_client_->send_command("split-window -h");
                else
                    tabs_->split_pane(SplitDir::Vertical);
                needs_render_ = true;
                return;
            case XKB_KEY_E:
            case XKB_KEY_e:
                if (tmux_controller_ && tmux_controller_->is_active())
                    tmux_client_->send_command("split-window -v");
                else
                    tabs_->split_pane(SplitDir::Horizontal);
                needs_render_ = true;
                return;
            case XKB_KEY_W:
            case XKB_KEY_w:
                if (tmux_controller_ && tmux_controller_->is_active()) {
                    tmux_client_->send_command("kill-pane");
                } else {
                    if (!tabs_->close_focused_pane()) {
                        if (on_close) on_close(this);
                    }
                }
                needs_render_ = true;
                return;
            // Pane navigation
            case XKB_KEY_Left:
                tabs_->navigate_pane(NavDir::Left);
                return;
            case XKB_KEY_Right:
                tabs_->navigate_pane(NavDir::Right);
                return;
            case XKB_KEY_Up:
                tabs_->navigate_pane(NavDir::Up);
                return;
            case XKB_KEY_Down:
                tabs_->navigate_pane(NavDir::Down);
                return;
            // New tab
            case XKB_KEY_T:
            case XKB_KEY_t:
                if (tmux_controller_ && tmux_controller_->is_active())
                    tmux_client_->send_command("new-window");
                else
                    tabs_->new_tab();
                recompute();
                needs_render_ = true;
                return;
        }
    }

    // Font size: Ctrl+plus/minus/0 (without Shift)
    if (ctrl && !shift) {
        switch (key.keysym) {
            case XKB_KEY_plus:
            case XKB_KEY_equal:
                config_.font_size += 1.0f;
                resize_font();
                return;
            case XKB_KEY_minus:
                if (config_.font_size > 6.0f) {
                    config_.font_size -= 1.0f;
                    resize_font();
                }
                return;
            case XKB_KEY_0:
                config_.font_size = 10.0f;
                resize_font();
                return;
        }
    }

    // Tab cycling: Ctrl+Tab / Ctrl+Shift+Tab
    if (ctrl && !shift && key.keysym == XKB_KEY_Tab) {
        tabs_->next_tab();
        recompute();
        needs_render_ = true;
        return;
    }
    if (ctrl && shift && key.keysym == XKB_KEY_ISO_Left_Tab) {
        tabs_->prev_tab();
        recompute();
        needs_render_ = true;
        return;
    }

    // Alt+1..9: switch to tab by index
    bool alt = key.mods & KeyMod::Alt;
    if (alt && key.keysym >= XKB_KEY_1 && key.keysym <= XKB_KEY_9) {
        int idx = key.keysym - XKB_KEY_1;
        if (idx < tabs_->tab_count()) {
            tabs_->activate_tab(idx);
            recompute();
            needs_render_ = true;
        }
        return;
    }

    // Shift+PageUp/Down for scrolling
    if (shift && key.keysym == XKB_KEY_Page_Up) {
        screen.scroll_viewport(-screen.rows() / 2);
        needs_render_ = true;
        return;
    }
    if (shift && key.keysym == XKB_KEY_Page_Down) {
        screen.scroll_viewport(screen.rows() / 2);
        needs_render_ = true;
        return;
    }

    // Forward to PTY
    std::string seq = encode_key(key, screen);
    if (!seq.empty()) {
        if (!screen.at_bottom()) {
            screen.scroll_to_bottom();
            needs_render_ = true;
        }
        pane->write(seq);
    }
}

void Window::handle_mouse(const MouseEvent &mouse) {
    Tab *tab = tabs_->active_tab();
    if (!tab) return;

    const auto &met = renderer_.metrics();
    int bar_h = tab_bar_height();

    // Tab bar click handling
    if (bar_h > 0 && mouse.y < bar_h) {
        if (mouse.pressed && !mouse.motion) {
            int hit = renderer_.tab_hit_test(*tabs_, mouse.x);
            if (hit >= 0) {
                if (mouse.button == MouseButton::Left) {
                    tabs_->activate_tab(hit);
                } else if (mouse.button == MouseButton::Middle) {
                    if (!tabs_->close_tab(hit)) {
                        if (on_close) on_close(this);
                    }
                }
                recompute();
                needs_render_ = true;
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
    if (!target_pane) return;

    // Focus follows mouse
    if (target_pane != tab->focused_pane) {
        tab->focused_pane = target_pane;
        target_pane->has_activity = false;
        needs_render_ = true;
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
            needs_render_ = true;
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
                if (!text.empty()) platform_->set_clipboard(text, true);
            } else if (target_pane->click_count == 3) {
                target_pane->selecting = false;
                const Line &line = screen.line(cell_row);
                screen.selection.active = true;
                screen.selection.start_line = abs_line;
                screen.selection.start_col = 0;
                screen.selection.end_line = abs_line;
                screen.selection.end_col = (int)line.cells.size() - 1;
                std::string text = screen.get_selection_text();
                if (!text.empty()) platform_->set_clipboard(text, true);
            }
            needs_render_ = true;
        } else if (mouse.motion && target_pane->selecting) {
            screen.selection.end_line = screen.absolute_line(cell_row);
            screen.selection.end_col = cell_col;
            needs_render_ = true;
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
                        platform_->set_clipboard(text, true);
                    }
                }
                needs_render_ = true;
            }
        } else if (mouse.button == MouseButton::Middle && mouse.pressed) {
            std::string text = platform_->get_clipboard(true);
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
    tmux_stale_controller_.reset();
    tmux_stale_client_.reset();

    if (!needs_render_) return;

    platform_->make_current();
    renderer_.begin_frame(config_);

    Tab *tab = tabs_->active_tab();
    if (tab) {
        int bar_h = tab_bar_height();

        // Render tab bar if multiple tabs
        if (bar_h > 0) {
            renderer_.render_tab_bar(*tabs_, config_, bar_h);
            renderer_.flush();
        }

        // Render dot grid in dead zone for tmux-managed tabs
        if (tab->tmux_managed) {
            const auto &m = renderer_.metrics();
            renderer_.render_dot_grid(0, bar_h, win_w_, win_h_ - bar_h,
                                       m.cell_width, m.cell_height);
            renderer_.flush();
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
            renderer_.render_pane(p->screen(), config_, r.x, r.y, r.w, r.h,
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
                            renderer_.render_border((float)border_x, (float)top_edge,
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
                            renderer_.render_border((float)left_edge, (float)border_y,
                                                   (float)(right_edge - left_edge), (float)border_h,
                                                   0.3f, 0.3f, 0.3f);
                        }
                    }
                }

                draw_borders(node->first.get());
                draw_borders(node->second.get());
            };

            draw_borders(tab->layout.root());
            renderer_.flush();
        }
    }

    platform_->swap_buffers();
    needs_render_ = false;
}

} // namespace rivt
