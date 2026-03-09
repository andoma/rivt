#include "platform/platform.h"
#include "core/event_loop.h"
#include "core/config.h"
#include "core/tab_manager.h"
#include "core/input_encoder.h"
#include "render/renderer.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <climits>
#include <signal.h>

using namespace rivt;

static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int) {
    got_sigchld = 1;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGCHLD, sigchld_handler);

    Config config;
    EventLoop loop;

    // Create platform (X11)
    auto platform = Platform::create();
    if (!platform) {
        fprintf(stderr, "Failed to create platform\n");
        return 1;
    }

    int win_w = 800, win_h = 600;
    if (!platform->create_window(win_w, win_h, "rivt")) {
        fprintf(stderr, "Failed to create window\n");
        return 1;
    }

    if (!platform->create_gl_context()) {
        fprintf(stderr, "Failed to create GL context\n");
        return 1;
    }

    // Initialize renderer
    Renderer renderer;
    if (!renderer.init(config)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return 1;
    }

    renderer.set_viewport(win_w, win_h);

    // Create tab manager
    TabManager tabs(config, loop, platform.get());

    bool needs_render = true;

    tabs.on_needs_render = [&]() { needs_render = true; };
    tabs.on_quit = [&]() { loop.request_quit(); };

    // Set cell dimensions and resize window to match initial_cols x initial_rows
    const auto &m = renderer.metrics();
    tabs.set_cell_size(m.cell_width, m.cell_height);
    win_w = config.initial_cols * m.cell_width;
    win_h = config.initial_rows * m.cell_height;
    platform->resize_window(win_w, win_h);
    renderer.set_viewport(win_w, win_h);

    // Compute tab bar height
    auto tab_bar_height = [&]() -> int {
        return tabs.tab_count() > 1 ? m.cell_height + 8 : 0;
    };

    // Recompute layout helper
    auto recompute = [&]() {
        int bar_h = tab_bar_height();
        tabs.recompute_layout(0, bar_h, win_w, win_h - bar_h);
    };

    // Resize font and adjust window to maintain current grid dimensions
    auto resize_font = [&]() {
        int cols, rows;
        renderer.compute_grid(win_w, win_h - tab_bar_height(), cols, rows);
        renderer.set_font_size(config.font_size, platform->get_dpi_scale() * 96.0f);
        tabs.set_cell_size(renderer.metrics().cell_width, renderer.metrics().cell_height);
        const auto &met = renderer.metrics();
        win_w = cols * met.cell_width;
        win_h = rows * met.cell_height + tab_bar_height();
        platform->resize_window(win_w, win_h);
        renderer.set_viewport(win_w, win_h);
        recompute();
        needs_render = true;
    };

    // Create first tab
    if (!tabs.new_tab()) {
        fprintf(stderr, "Failed to spawn initial shell\n");
        return 1;
    }
    recompute();

    // Search helpers for focused pane
    auto search_navigate = [&](int delta) {
        Pane *pane = tabs.focused_pane();
        if (!pane) return;
        auto &s = pane->screen().search;
        if (s.matches.empty()) return;
        s.current_match = (s.current_match + delta + (int)s.matches.size()) % (int)s.matches.size();
        const auto &match = s.matches[s.current_match];
        int rows = pane->screen().rows();
        int vis_row = match.abs_line - pane->screen().absolute_line(0);
        if (vis_row < 0 || vis_row >= rows) {
            int target_offset = -(int)pane->screen().scrollback_count() + match.abs_line - rows / 2;
            pane->screen().scroll_viewport(target_offset - pane->screen().viewport_offset());
        }
        needs_render = true;
    };

    auto search_update = [&]() {
        Pane *pane = tabs.focused_pane();
        if (!pane) return;
        pane->screen().find_matches(pane->screen().search.query, pane->screen().search.case_sensitive);
        needs_render = true;
    };

    // Keyboard handler
    platform->on_key = [&](const KeyEvent &key) {
        if (!key.pressed) return;

        Pane *pane = tabs.focused_pane();
        if (!pane) return;

        ScreenBuffer &screen = pane->screen();
        bool ctrl  = key.mods & KeyMod::Ctrl;
        bool shift = key.mods & KeyMod::Shift;

        // Search mode input handling
        if (screen.search.focused) {
            switch (key.keysym) {
                case XKB_KEY_Escape:
                    screen.search.focused = false;
                    needs_render = true;
                    return;
                case XKB_KEY_Return:
                    search_navigate(shift ? -1 : 1);
                    return;
                case XKB_KEY_BackSpace:
                    if (!screen.search.query.empty()) {
                        screen.search.query.pop_back();
                        search_update();
                    }
                    return;
                default:
                    if (!key.text.empty() && key.text[0] >= ' ') {
                        screen.search.query += key.text;
                        search_update();
                    }
                    return;
            }
        }

        // Escape when search is active but unfocused: close search
        if (key.keysym == XKB_KEY_Escape && screen.search.active) {
            screen.search.clear();
            needs_render = true;
            return;
        }

        // Internal shortcuts
        if (ctrl && shift) {
            switch (key.keysym) {
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
                    needs_render = true;
                    return;
                case XKB_KEY_V:
                case XKB_KEY_v: {
                    std::string text = platform->get_clipboard(false);
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
                        platform->set_clipboard(text, false);
                    }
                    return;
                }
                case XKB_KEY_plus:
                case XKB_KEY_equal:
                    config.font_size += 1.0f;
                    resize_font();
                    return;
                case XKB_KEY_minus:
                    if (config.font_size > 6.0f) {
                        config.font_size -= 1.0f;
                        resize_font();
                    }
                    return;
                case XKB_KEY_0:
                    config.font_size = 10.0f;
                    resize_font();
                    return;
                // Pane splits
                case XKB_KEY_D:
                case XKB_KEY_d:
                    tabs.split_pane(SplitDir::Vertical);
                    needs_render = true;
                    return;
                case XKB_KEY_E:
                case XKB_KEY_e:
                    tabs.split_pane(SplitDir::Horizontal);
                    needs_render = true;
                    return;
                case XKB_KEY_W:
                case XKB_KEY_w:
                    if (!tabs.close_focused_pane()) {
                        loop.request_quit();
                    }
                    needs_render = true;
                    return;
                // Pane navigation
                case XKB_KEY_Left:
                    tabs.navigate_pane(NavDir::Left);
                    return;
                case XKB_KEY_Right:
                    tabs.navigate_pane(NavDir::Right);
                    return;
                case XKB_KEY_Up:
                    tabs.navigate_pane(NavDir::Up);
                    return;
                case XKB_KEY_Down:
                    tabs.navigate_pane(NavDir::Down);
                    return;
                // New tab
                case XKB_KEY_T:
                case XKB_KEY_t:
                    tabs.new_tab();
                    recompute();
                    needs_render = true;
                    return;
            }
        }

        // Font size: Ctrl+plus/minus/0 (without Shift)
        if (ctrl && !shift) {
            switch (key.keysym) {
                case XKB_KEY_plus:
                case XKB_KEY_equal:
                    config.font_size += 1.0f;
                    resize_font();
                    return;
                case XKB_KEY_minus:
                    if (config.font_size > 6.0f) {
                        config.font_size -= 1.0f;
                        resize_font();
                    }
                    return;
                case XKB_KEY_0:
                    config.font_size = 10.0f;
                    resize_font();
                    return;
            }
        }

        // Tab cycling: Ctrl+Tab / Ctrl+Shift+Tab
        if (ctrl && !shift && key.keysym == XKB_KEY_Tab) {
            tabs.next_tab();
            recompute();
            needs_render = true;
            return;
        }
        if (ctrl && shift && key.keysym == XKB_KEY_ISO_Left_Tab) {
            tabs.prev_tab();
            recompute();
            needs_render = true;
            return;
        }

        // Alt+1..9: switch to tab by index
        bool alt = key.mods & KeyMod::Alt;
        if (alt && key.keysym >= XKB_KEY_1 && key.keysym <= XKB_KEY_9) {
            int idx = key.keysym - XKB_KEY_1;
            if (idx < tabs.tab_count()) {
                tabs.activate_tab(idx);
                recompute();
                needs_render = true;
            }
            return;
        }

        // Shift+PageUp/Down for scrolling
        if (shift && key.keysym == XKB_KEY_Page_Up) {
            screen.scroll_viewport(-screen.rows() / 2);
            needs_render = true;
            return;
        }
        if (shift && key.keysym == XKB_KEY_Page_Down) {
            screen.scroll_viewport(screen.rows() / 2);
            needs_render = true;
            return;
        }

        // Forward to PTY
        std::string seq = encode_key(key, screen);
        if (!seq.empty()) {
            if (!screen.at_bottom()) {
                screen.scroll_to_bottom();
                needs_render = true;
            }
            pane->write(seq);
        }
    };

    // Mouse handler
    platform->on_mouse = [&](const MouseEvent &mouse) {
        Tab *tab = tabs.active_tab();
        if (!tab) return;

        const auto &met = renderer.metrics();
        int bar_h = tab_bar_height();

        // Tab bar click handling
        if (bar_h > 0 && mouse.y < bar_h) {
            if (mouse.pressed && !mouse.motion) {
                int hit = renderer.tab_hit_test(tabs, mouse.x);
                if (hit >= 0) {
                    if (mouse.button == MouseButton::Left) {
                        tabs.activate_tab(hit);
                    } else if (mouse.button == MouseButton::Middle) {
                        if (!tabs.close_tab(hit)) {
                            loop.request_quit();
                        }
                    }
                    recompute();
                    needs_render = true;
                }
            }
            return;
        }

        // Route mouse to correct pane
        Pane *target_pane = tab->layout.pane_at(mouse.x, mouse.y);
        if (!target_pane) return;

        // Focus follows mouse
        if (target_pane != tab->focused_pane) {
            tab->focused_pane = target_pane;
            target_pane->has_activity = false;
            needs_render = true;
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
                needs_render = true;
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
                    if (!text.empty()) platform->set_clipboard(text, true);
                } else if (target_pane->click_count == 3) {
                    target_pane->selecting = false;
                    const Line &line = screen.line(cell_row);
                    screen.selection.active = true;
                    screen.selection.start_line = abs_line;
                    screen.selection.start_col = 0;
                    screen.selection.end_line = abs_line;
                    screen.selection.end_col = (int)line.cells.size() - 1;
                    std::string text = screen.get_selection_text();
                    if (!text.empty()) platform->set_clipboard(text, true);
                }
                needs_render = true;
            } else if (mouse.motion && target_pane->selecting) {
                screen.selection.end_line = screen.absolute_line(cell_row);
                screen.selection.end_col = cell_col;
                needs_render = true;
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
                            platform->set_clipboard(text, true);
                        }
                    }
                    needs_render = true;
                }
            } else if (mouse.button == MouseButton::Middle && mouse.pressed) {
                std::string text = platform->get_clipboard(true);
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
    };

    platform->on_resize = [&](int width, int height) {
        win_w = width;
        win_h = height;
        renderer.set_viewport(width, height);
        tabs.set_cell_size(renderer.metrics().cell_width, renderer.metrics().cell_height);
        recompute();
        needs_render = true;
    };

    platform->on_focus = [&](bool focused) {
        renderer.set_focused(focused);
        needs_render = true;
        Pane *pane = tabs.focused_pane();
        if (pane && pane->screen().focus_reporting()) {
            pane->write(focused ? "\033[I" : "\033[O");
        }
    };

    platform->on_close = [&]() {
        loop.request_quit();
    };

    // Add platform fd to event loop
    loop.add_fd(platform->get_event_fd(), [&](uint32_t) {
        platform->process_events();
    });

    // Cursor blink timer
    bool cursor_blink_on = true;
    loop.add_timer(600, [&]() {
        cursor_blink_on = !cursor_blink_on;
        needs_render = true;
    });

    // Main loop
    while (!loop.should_quit()) {
        loop.poll(needs_render ? 0 : 16);

        if (got_sigchld) {
            got_sigchld = 0;
            if (!tabs.reap_dead_panes()) {
                loop.request_quit();
            }
            recompute();
            needs_render = true;
        }

        if (needs_render) {
            platform->make_current();
            renderer.begin_frame(config);

            Tab *tab = tabs.active_tab();
            if (tab) {
                int bar_h = tab_bar_height();

                // Render tab bar if multiple tabs
                if (bar_h > 0) {
                    renderer.render_tab_bar(tabs, config, bar_h);
                    renderer.flush();
                }

                // Render each pane with scissor clipping
                std::vector<Pane *> panes;
                tab->layout.collect_panes(panes);

                for (Pane *p : panes) {
                    const auto &r = p->rect;
                    renderer.render_pane(p->screen(), config, r.x, r.y, r.w, r.h,
                                         p == tab->focused_pane);
                }

                // Render borders between panes (over the top, no scissor)
                if (panes.size() > 1) {
                    // Walk the layout tree and draw borders at split points
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
                                    renderer.render_border((float)border_x, (float)top_edge,
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
                                    renderer.render_border((float)left_edge, (float)border_y,
                                                           (float)(right_edge - left_edge), (float)border_h,
                                                           0.3f, 0.3f, 0.3f);
                                }
                            }
                        }

                        draw_borders(node->first.get());
                        draw_borders(node->second.get());
                    };

                    draw_borders(tab->layout.root());
                    renderer.flush();
                }
            }

            platform->swap_buffers();
            needs_render = false;
        }
    }

    return 0;
}
