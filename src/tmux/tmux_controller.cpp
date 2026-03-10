#include "tmux/tmux_controller.h"
#include "core/window.h"
#include "core/tab_manager.h"
#include "core/tab.h"
#include "core/pane.h"
#include "core/debug.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>

namespace rivt {

TmuxController::TmuxController(TmuxClient &client, Window &window,
                                 TabManager &tabs, EventLoop &loop)
    : client_(client), window_(window), tabs_(tabs), loop_(loop)
{
    client_.on_output = [this](int id, const std::string &d) { on_output(id, d); };
    client_.on_window_add = [this](int id) { on_window_add(id); };
    client_.on_window_close = [this](int id) { on_window_close(id); };
    client_.on_window_renamed = [this](int id, const std::string &n) { on_window_renamed(id, n); };
    client_.on_layout_change = [this](int id, const std::string &l, bool active) { on_layout_change(id, l, active); };
    client_.on_session_changed = [this]() { on_session_changed(); };
    client_.on_session_window_changed = [this](int id) { on_session_window_changed(id); };
    client_.on_exit = [this]() { on_exit(); };
}

void TmuxController::initialize(int cols, int rows, int cell_w, int cell_h,
                                  int content_x, int content_y) {
    active_ = true;
    cell_w_ = cell_w;
    cell_h_ = cell_h;
    content_x_ = content_x;
    content_y_ = content_y;
    dbg("tmux: initialize cell=%dx%d content_origin=%d,%d cols=%d rows=%d",
        cell_w, cell_h, content_x, content_y, cols, rows);

    // Send initial client size so tmux knows our dimensions and sends
    // layout-change notifications for all windows in the session.
    // In PTY mode, defer this — %session-changed will trigger list-windows.
    // Sending commands before tmux is ready risks them reaching the shell
    // if tmux exits immediately (e.g., no session to attach to).
    if (cols > 0 && rows > 0 && !client_.is_pty_mode()) {
        client_.refresh_client_size(cols, rows);
    }
}

void TmuxController::handle_resize(int cols, int rows, int cell_w, int cell_h,
                                     int content_x, int content_y) {
    if (!active_) return;
    dbg("tmux: handle_resize cols=%d rows=%d cell=%dx%d content_origin=%d,%d",
        cols, rows, cell_w, cell_h, content_x, content_y);
    cell_w_ = cell_w;
    cell_h_ = cell_h;
    content_x_ = content_x;
    content_y_ = content_y;
    client_.refresh_client_size(cols, rows);
}

void TmuxController::detach() {
    if (!active_) return;
    client_.detach();
    active_ = false;
}

void TmuxController::on_output(int pane_id, const std::string &data) {
    auto it = pane_map_.find(pane_id);
    if (it != pane_map_.end()) {
        dbg("tmux: output pane=%%%d len=%zu -> feed_data", pane_id, data.size());
        it->second->feed_data(data.data(), data.size());
    } else {
        dbg("tmux: output pane=%%%d len=%zu -> buffered (pane not yet created)", pane_id, data.size());
        output_buffer_[pane_id] += data;
    }
}

void TmuxController::on_window_add(int window_id) {
    dbg("tmux: window-add @%d (already_known=%d)", window_id, (int)window_map_.count(window_id));
    if (window_map_.count(window_id)) return;

    // Don't create the tab yet — wait for %layout-change which has the
    // actual pane geometries.  Request the layout explicitly in case
    // %layout-change doesn't arrive on its own.
    client_.send_command(
        "list-windows -F '#{window_id} #{window_layout}'",
        [this, window_id](const std::string &output) {
            dbg("tmux: window-add list-windows response (%zu bytes)", output.size());
            size_t pos = 0;
            while (pos < output.size()) {
                size_t nl = output.find('\n', pos);
                if (nl == std::string::npos) nl = output.size();
                std::string line = output.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.empty()) continue;
                size_t sp = line.find(' ');
                if (sp == std::string::npos || sp == 0) continue;
                std::string id_str = line.substr(0, sp);
                if (!id_str.empty() && id_str[0] == '@') id_str = id_str.substr(1);
                if (id_str.empty()) continue;
                int wid = std::stoi(id_str);
                if (wid != window_id) continue;
                std::string layout = line.substr(sp + 1);
                dbg("tmux: window-add: @%d layout='%s'", wid, layout.c_str());
                on_layout_change(wid, layout, true);
                request_window_names();
                return;
            }
            dbg("tmux: window-add: @%d not found in list-windows", window_id);
        });
}

void TmuxController::on_window_close(int window_id) {
    dbg("tmux: window-close @%d", window_id);
    auto it = window_map_.find(window_id);
    if (it == window_map_.end()) return;

    Tab *tab = it->second;

    // Remove all panes belonging to this window from pane_map_
    for (auto pit = pane_map_.begin(); pit != pane_map_.end(); ) {
        bool found = false;
        for (auto &p : tab->panes) {
            if (p.get() == pit->second) { found = true; break; }
        }
        if (found) pit = pane_map_.erase(pit);
        else ++pit;
    }

    if (!tabs_.close_tab_ptr(tab)) {
        // Last tab closed — window should close
        window_.mark_closing();
    }
    window_map_.erase(it);

    // Tab bar may have appeared/disappeared — reposition all panes
    reposition_all_panes();
}

void TmuxController::on_window_renamed(int window_id, const std::string &name) {
    dbg("tmux: window-renamed @%d -> '%s'", window_id, name.c_str());
    auto it = window_map_.find(window_id);
    if (it != window_map_.end()) {
        it->second->title = name;
        if (tabs_.on_needs_render) tabs_.on_needs_render();
    }
}

void TmuxController::on_layout_change(int window_id, const std::string &layout_str, bool is_active) {
    dbg("tmux: layout-change @%d layout='%s' active=%d", window_id, layout_str.c_str(), is_active);
    auto wit = window_map_.find(window_id);
    Tab *tab;
    if (wit == window_map_.end()) {
        dbg("tmux: layout-change: creating new tab for @%d", window_id);
        tab = tabs_.new_empty_tab("tmux");
        tab->tmux_managed = true;
        window_map_[window_id] = tab;
    } else {
        tab = wit->second;
    }

    auto geoms = parse_layout(layout_str);
    dbg("tmux: layout-change: parsed %zu pane geometries", geoms.size());
    if (geoms.empty()) return;

    // On first layout, resize the window to fit the tmux session
    if (!initial_resize_done_ && cell_w_ > 0 && cell_h_ > 0) {
        initial_resize_done_ = true;
        int max_col = 0, max_row = 0;
        for (auto &g : geoms) {
            max_col = std::max(max_col, g.x + g.w);
            max_row = std::max(max_row, g.y + g.h);
        }
        dbg("tmux: initial resize to %dx%d cells", max_col, max_row);
        if (max_col > 0 && max_row > 0) {
            window_.resize_to_cells(max_col, max_row);
        }
    }

    // Tab bar may have appeared/disappeared — reposition all existing panes
    reposition_all_panes();
    dbg("tmux: content_y_=%d (tab_bar_height)", content_y_);

    // Determine which panes are new, existing, or gone
    std::unordered_map<int, Pane *> current_panes;
    for (auto &[pid, pane] : pane_map_) {
        // Check if this pane belongs to this tab
        for (auto &p : tab->panes) {
            if (p.get() == pane) {
                current_panes[pid] = pane;
                break;
            }
        }
    }

    std::unordered_map<int, Pane *> new_pane_set;

    for (auto &g : geoms) {
        auto it = current_panes.find(g.pane_id);
        if (it != current_panes.end()) {
            // Existing pane — update geometry
            Pane *pane = it->second;
            pane->rect.x = content_x_ + g.x * cell_w_;
            pane->rect.y = content_y_ + g.y * cell_h_;
            pane->rect.w = g.w * cell_w_;
            pane->rect.h = g.h * cell_h_;
            pane->resize(g.w, g.h);
            dbg("tmux: pane %%%d update rect=(%d,%d %dx%d) cells=%dx%d",
                g.pane_id, pane->rect.x, pane->rect.y, pane->rect.w, pane->rect.h, g.w, g.h);
            new_pane_set[g.pane_id] = pane;
            current_panes.erase(it);
        } else {
            // New pane
            Pane *pane = create_tmux_pane(tab, g.pane_id, g.w, g.h);
            if (pane) {
                pane->rect.x = content_x_ + g.x * cell_w_;
                pane->rect.y = content_y_ + g.y * cell_h_;
                pane->rect.w = g.w * cell_w_;
                pane->rect.h = g.h * cell_h_;
                dbg("tmux: pane %%%d NEW rect=(%d,%d %dx%d) cells=%dx%d",
                    g.pane_id, pane->rect.x, pane->rect.y, pane->rect.w, pane->rect.h, g.w, g.h);
                new_pane_set[g.pane_id] = pane;

                // Replay buffered output
                auto buf_it = output_buffer_.find(g.pane_id);
                if (buf_it != output_buffer_.end()) {
                    pane->feed_data(buf_it->second.data(), buf_it->second.size());
                    output_buffer_.erase(buf_it);
                }
            }
        }
    }

    // Remove panes that are gone
    for (auto &[pid, pane] : current_panes) {
        pane_map_.erase(pid);
        tabs_.remove_pane(tab, pane);
    }

    // Activate this tab if tmux says it's the active window
    if (is_active) {
        for (int i = 0; i < tabs_.tab_count(); i++) {
            if (tabs_.tabs()[i].get() == tab) {
                tabs_.activate_tab(i);
                break;
            }
        }
    }

    if (tabs_.on_needs_render) tabs_.on_needs_render();
}

Pane *TmuxController::create_tmux_pane(Tab *tab, int tmux_pane_id, int cols, int rows) {
    dbg("tmux: create_tmux_pane %%%d %dx%d", tmux_pane_id, cols, rows);
    Pane *pane = tabs_.add_pane_to_tab(tab, cols, rows);
    if (!pane) { dbg("tmux: add_pane_to_tab returned null!"); return nullptr; }

    pane->write_callback_ = [this, tmux_pane_id](const std::string &data) {
        client_.send_keys(tmux_pane_id, data);
    };

    pane_map_[tmux_pane_id] = pane;

    // Request current pane content so the display isn't blank
    client_.send_command(
        "capture-pane -e -p -t %" + std::to_string(tmux_pane_id),
        [pane](const std::string &output) {
            if (output.empty()) return;
            // Cursor home + clear screen, then paint captured content
            std::string data = "\033[H\033[J";
            size_t pos = 0;
            while (pos < output.size()) {
                size_t nl = output.find('\n', pos);
                if (nl == std::string::npos) {
                    data += output.substr(pos);
                    break;
                }
                data += output.substr(pos, nl - pos);
                if (nl + 1 < output.size()) data += "\r\n";
                pos = nl + 1;
            }
            pane->feed_data(data.data(), data.size());
        });

    return pane;
}

void TmuxController::on_session_changed() {
    dbg("tmux: session-changed, requesting window list");
    client_.send_command(
        "list-windows -F '#{window_id} #{window_layout}'",
        [this](const std::string &output) {
            dbg("tmux: list-windows response (%zu bytes)", output.size());
            size_t pos = 0;
            while (pos < output.size()) {
                size_t nl = output.find('\n', pos);
                if (nl == std::string::npos) nl = output.size();
                std::string line = output.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.empty()) continue;

                size_t sp = line.find(' ');
                if (sp == std::string::npos) continue;
                std::string id_str = line.substr(0, sp);
                if (!id_str.empty() && id_str[0] == '@') id_str = id_str.substr(1);
                if (id_str.empty()) continue;
                int window_id = std::stoi(id_str);
                std::string layout = line.substr(sp + 1);
                dbg("tmux: list-windows: @%d layout='%s'", window_id, layout.c_str());
                on_layout_change(window_id, layout);
            }
            // After layouts are set up, fetch window names
            request_window_names();
        });
}

void TmuxController::request_window_names() {
    client_.send_command(
        "list-windows -F '#{window_id} #{window_active} #{window_name} #{pane_title}'",
        [this](const std::string &output) {
            size_t pos = 0;
            while (pos < output.size()) {
                size_t nl = output.find('\n', pos);
                if (nl == std::string::npos) nl = output.size();
                std::string line = output.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.empty()) continue;

                // Parse: @ID ACTIVE WINDOW_NAME PANE_TITLE
                size_t sp1 = line.find(' ');
                if (sp1 == std::string::npos) continue;
                std::string id_str = line.substr(0, sp1);
                if (!id_str.empty() && id_str[0] == '@') id_str = id_str.substr(1);
                if (id_str.empty()) continue;
                int window_id = std::stoi(id_str);

                size_t sp2 = line.find(' ', sp1 + 1);
                if (sp2 == std::string::npos) continue;
                bool active = line.substr(sp1 + 1, sp2 - sp1 - 1) == "1";

                size_t sp3 = line.find(' ', sp2 + 1);
                std::string window_name;
                std::string pane_title;
                if (sp3 == std::string::npos) {
                    window_name = line.substr(sp2 + 1);
                } else {
                    window_name = line.substr(sp2 + 1, sp3 - sp2 - 1);
                    pane_title = line.substr(sp3 + 1);
                }

                dbg("tmux: window @%d name='%s' pane_title='%s' active=%d",
                    window_id, window_name.c_str(), pane_title.c_str(), active);

                // Tab title: just the short window name
                on_window_renamed(window_id, window_name);

                // X11 window title: full pane title for active window
                if (active && !pane_title.empty()) {
                    window_.platform()->set_title("rivt [tmux] " + pane_title);
                }
            }
        });
}

void TmuxController::reposition_all_panes() {
    int new_y = window_.tab_bar_height();
    if (new_y == content_y_) return;
    dbg("tmux: reposition_all_panes content_y %d -> %d", content_y_, new_y);
    int delta = new_y - content_y_;
    content_y_ = new_y;
    for (auto &[wid, tab] : window_map_) {
        for (auto &p : tab->panes) {
            p->rect.y += delta;
        }
    }
    if (tabs_.on_needs_render) tabs_.on_needs_render();
}

void TmuxController::on_session_window_changed(int window_id) {
    dbg("tmux: session-window-changed @%d", window_id);
    auto it = window_map_.find(window_id);
    if (it == window_map_.end()) return;
    Tab *tab = it->second;
    for (int i = 0; i < tabs_.tab_count(); i++) {
        if (tabs_.tabs()[i].get() == tab) {
            tabs_.activate_tab(i);
            break;
        }
    }
}

void TmuxController::on_exit() {
    dbg("tmux: on_exit (gateway_pane_=%p)", (void*)gateway_pane_);
    active_ = false;

    if (gateway_pane_) {
        // PTY mode: clean up tmux tabs/panes, restore gateway pane
        // Close all tmux-managed tabs
        for (auto &[wid, tab] : window_map_) {
            tabs_.close_tab_ptr(tab);
        }
        window_map_.clear();
        pane_map_.clear();
        output_buffer_.clear();

        // Notify Window to restore normal operation
        if (on_tmux_exit) on_tmux_exit();
    } else {
        // Subprocess mode: close the window
        window_.mark_closing();
    }
}

// tmux layout string parser
// Format: checksum,WxH,X,Y{...} or [...] or PANE_ID
// Examples:
//   "bb62,80x24,0,0,0"  (single pane, id=0)
//   "a]a5,159x47,0,0{79x47,0,0,0,79x47,80,0,1}" (vertical split)
//   "63e4,159x47,0,0[159x23,0,0,0,159x23,0,24,1]" (horizontal split)

static bool parse_layout_node(const std::string &s, size_t &pos,
                               std::vector<TmuxController::PaneGeom> &out) {
    // Parse: WxH,X,Y,PANE_ID or WxH,X,Y{...} or WxH,X,Y[...]
    // First: dimensions WxH
    size_t wx = s.find('x', pos);
    if (wx == std::string::npos) return false;
    int w = std::stoi(s.substr(pos, wx - pos));
    pos = wx + 1;

    size_t comma1 = s.find(',', pos);
    if (comma1 == std::string::npos) return false;
    int h = std::stoi(s.substr(pos, comma1 - pos));
    pos = comma1 + 1;

    // X
    size_t comma2 = s.find(',', pos);
    if (comma2 == std::string::npos) return false;
    int x = std::stoi(s.substr(pos, comma2 - pos));
    pos = comma2 + 1;

    // Y - followed by comma+pane_id, or { or [
    // Find next delimiter
    size_t delim = pos;
    while (delim < s.size() && s[delim] != ',' && s[delim] != '{' && s[delim] != '[' &&
           s[delim] != '}' && s[delim] != ']')
        delim++;

    int y = std::stoi(s.substr(pos, delim - pos));
    pos = delim;

    if (pos >= s.size() || s[pos] == '}' || s[pos] == ']') {
        // Shouldn't happen (no pane_id), but treat as error
        return false;
    }

    if (s[pos] == '{' || s[pos] == '[') {
        // Container: recurse into children
        char open = s[pos];
        char close = (open == '{') ? '}' : ']';
        pos++; // skip open bracket

        while (pos < s.size() && s[pos] != close) {
            if (s[pos] == ',') pos++; // skip separator between children
            if (!parse_layout_node(s, pos, out)) return false;
        }
        if (pos < s.size()) pos++; // skip close bracket
        return true;
    }

    if (s[pos] == ',') {
        pos++; // skip comma before pane_id
        size_t id_end = pos;
        while (id_end < s.size() && std::isdigit(s[id_end])) id_end++;
        if (id_end == pos) return false;
        int pane_id = std::stoi(s.substr(pos, id_end - pos));
        pos = id_end;
        out.push_back({pane_id, x, y, w, h});
        return true;
    }

    return false;
}

std::vector<TmuxController::PaneGeom> TmuxController::parse_layout(const std::string &layout) {
    std::vector<PaneGeom> result;

    // Skip checksum (format: XXXX,...)
    size_t start = layout.find(',');
    if (start == std::string::npos) return result;
    start++;

    size_t pos = start;
    parse_layout_node(layout, pos, result);
    return result;
}

} // namespace rivt
