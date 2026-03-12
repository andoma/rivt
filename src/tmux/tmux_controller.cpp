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
    : m_client(client), m_window(window), m_tabs(tabs), m_loop(loop)
{
    m_client.on_output = [this](int id, const std::string &d) { on_output(id, d); };
    m_client.on_window_add = [this](int id) { on_window_add(id); };
    m_client.on_window_close = [this](int id) { on_window_close(id); };
    m_client.on_window_renamed = [this](int id, const std::string &n) { on_window_renamed(id, n); };
    m_client.on_layout_change = [this](int id, const std::string &l, bool active) { on_layout_change(id, l, active); };
    m_client.on_session_changed = [this]() { on_session_changed(); };
    m_client.on_session_window_changed = [this](int id) { on_session_window_changed(id); };
    m_client.on_exit = [this]() { on_exit(); };
}

void TmuxController::initialize(int cols, int rows, int cell_w, int cell_h,
                                  int content_x, int content_y) {
    m_active = true;
    m_cell_w = cell_w;
    m_cell_h = cell_h;
    m_content_x = content_x;
    m_content_y = content_y;
    dbg("tmux: initialize cell=%dx%d content_origin=%d,%d cols=%d rows=%d",
        cell_w, cell_h, content_x, content_y, cols, rows);

    // In PTY mode, the tmux window (B) should keep its own size and tell
    // tmux to adapt — never resize to match the session layout.  Mark the
    // initial resize as done so on_layout_change won't call resize_to_cells.
    if (m_client.is_pty_mode()) {
        m_initial_resize_done = true;
    }

    // Send initial client size so tmux knows our dimensions and sends
    // layout-change notifications for all windows in the session.
    // In PTY mode, defer this — %session-changed will trigger list-windows.
    // Sending commands before tmux is ready risks them reaching the shell
    // if tmux exits immediately (e.g., no session to attach to).
    if (cols > 0 && rows > 0 && !m_client.is_pty_mode()) {
        m_client.refresh_client_size(cols, rows);
    }
}

void TmuxController::handle_resize(int cols, int rows, int cell_w, int cell_h,
                                     int content_x, int content_y) {
    if (!m_active) return;
    dbg("tmux: handle_resize cols=%d rows=%d cell=%dx%d content_origin=%d,%d",
        cols, rows, cell_w, cell_h, content_x, content_y);
    m_cell_w = cell_w;
    m_cell_h = cell_h;
    m_content_x = content_x;
    m_content_y = content_y;
    m_client.refresh_client_size(cols, rows);
}

void TmuxController::detach() {
    if (!m_active) return;
    m_client.detach();
    m_active = false;
}

void TmuxController::on_output(int pane_id, const std::string &data) {
    auto it = m_pane_map.find(pane_id);
    if (it != m_pane_map.end()) {
        dbg("tmux: output pane=%%%d len=%zu -> feed_data", pane_id, data.size());
        it->second->feed_data(data.data(), data.size());
    } else {
        dbg("tmux: output pane=%%%d len=%zu -> buffered (pane not yet created)", pane_id, data.size());
        m_output_buffer[pane_id] += data;
    }
}

void TmuxController::on_window_add(int window_id) {
    dbg("tmux: window-add @%d (already_known=%d)", window_id, (int)m_window_map.count(window_id));
    if (m_window_map.count(window_id)) return;

    // Don't create the tab yet — wait for %layout-change which has the
    // actual pane geometries.  Request the layout explicitly in case
    // %layout-change doesn't arrive on its own.
    m_client.send_command(
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
                save_affinities();
                return;
            }
            dbg("tmux: window-add: @%d not found in list-windows", window_id);
        });
}

void TmuxController::on_window_close(int window_id) {
    dbg("tmux: window-close @%d", window_id);
    auto it = m_window_map.find(window_id);
    if (it == m_window_map.end()) return;

    Tab *tab = it->second;

    // Remove all panes belonging to this window from m_pane_map
    for (auto pit = m_pane_map.begin(); pit != m_pane_map.end(); ) {
        bool found = false;
        for (auto &p : tab->panes) {
            if (p.get() == pit->second) { found = true; break; }
        }
        if (found) pit = m_pane_map.erase(pit);
        else ++pit;
    }

    if (!m_tabs.close_tab_ptr(tab)) {
        // Last tab closed — window should close
        m_window.mark_closing();
    }
    m_window_map.erase(it);
    save_affinities();

    // Tab bar may have appeared/disappeared — reposition all panes
    reposition_all_panes();
}

void TmuxController::on_window_renamed(int window_id, const std::string &name) {
    dbg("tmux: window-renamed @%d -> '%s'", window_id, name.c_str());
    auto it = m_window_map.find(window_id);
    if (it != m_window_map.end()) {
        it->second->title = name;
        if (m_tabs.on_needs_render) m_tabs.on_needs_render();
    }
}

void TmuxController::on_layout_change(int window_id, const std::string &layout_str, bool is_active) {
    dbg("tmux: layout-change @%d layout='%s' active=%d", window_id, layout_str.c_str(), is_active);
    auto wit = m_window_map.find(window_id);
    Tab *tab;
    bool new_tab = false;
    if (wit == m_window_map.end()) {
        dbg("tmux: layout-change: creating new tab for @%d", window_id);
        tab = m_tabs.new_empty_tab("tmux");
        tab->tmux_managed = true;
        m_window_map[window_id] = tab;
        new_tab = true;
    } else {
        tab = wit->second;
    }

    auto geoms = parse_layout(layout_str);
    dbg("tmux: layout-change: parsed %zu pane geometries", geoms.size());
    if (geoms.empty()) return;

    // On first layout, resize the window to fit the tmux session
    if (!m_initial_resize_done && m_cell_w > 0 && m_cell_h > 0) {
        m_initial_resize_done = true;
        int max_col = 0, max_row = 0;
        for (auto &g : geoms) {
            max_col = std::max(max_col, g.x + g.w);
            max_row = std::max(max_row, g.y + g.h);
        }
        dbg("tmux: initial resize to %dx%d cells", max_col, max_row);
        if (max_col > 0 && max_row > 0) {
            m_window.resize_to_cells(max_col, max_row);
        }
    }

    // Tab bar may have appeared/disappeared — reposition all existing panes
    reposition_all_panes();
    dbg("tmux: m_content_y=%d (tab_bar_height)", m_content_y);

    // Determine which panes are new, existing, or gone
    std::unordered_map<int, Pane *> current_panes;
    for (auto &[pid, pane] : m_pane_map) {
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
            pane->rect.x = m_content_x + g.x * m_cell_w;
            pane->rect.y = m_content_y + g.y * m_cell_h;
            pane->rect.w = g.w * m_cell_w;
            pane->rect.h = g.h * m_cell_h;
            pane->resize(g.w, g.h);
            dbg("tmux: pane %%%d update rect=(%d,%d %dx%d) cells=%dx%d",
                g.pane_id, pane->rect.x, pane->rect.y, pane->rect.w, pane->rect.h, g.w, g.h);
            new_pane_set[g.pane_id] = pane;
            current_panes.erase(it);
        } else {
            // New pane
            Pane *pane = create_tmux_pane(tab, g.pane_id, g.w, g.h);
            if (pane) {
                pane->rect.x = m_content_x + g.x * m_cell_w;
                pane->rect.y = m_content_y + g.y * m_cell_h;
                pane->rect.w = g.w * m_cell_w;
                pane->rect.h = g.h * m_cell_h;
                dbg("tmux: pane %%%d NEW rect=(%d,%d %dx%d) cells=%dx%d",
                    g.pane_id, pane->rect.x, pane->rect.y, pane->rect.w, pane->rect.h, g.w, g.h);
                new_pane_set[g.pane_id] = pane;

                // Replay buffered output
                auto buf_it = m_output_buffer.find(g.pane_id);
                if (buf_it != m_output_buffer.end()) {
                    pane->feed_data(buf_it->second.data(), buf_it->second.size());
                    m_output_buffer.erase(buf_it);
                }
            }
        }
    }

    // Remove panes that are gone
    for (auto &[pid, pane] : current_panes) {
        m_pane_map.erase(pid);
        m_tabs.remove_pane(tab, pane);
    }

    // Activate this tab if it's new and tmux says it's the active window.
    // Don't re-activate existing tabs — layout-change fires on resize too,
    // and we don't want to yank the user away from the tab they're viewing.
    if (is_active && new_tab) {
        for (int i = 0; i < m_tabs.tab_count(); i++) {
            if (m_tabs.tabs()[i].get() == tab) {
                m_tabs.activate_tab(i);
                break;
            }
        }
    }

    if (m_tabs.on_needs_render) m_tabs.on_needs_render();
}

Pane *TmuxController::create_tmux_pane(Tab *tab, int tmux_pane_id, int cols, int rows) {
    dbg("tmux: create_tmux_pane %%%d %dx%d", tmux_pane_id, cols, rows);
    Pane *pane = m_tabs.add_pane_to_tab(tab, cols, rows);
    if (!pane) { dbg("tmux: add_pane_to_tab returned null!"); return nullptr; }

    pane->m_write_callback = [this, tmux_pane_id](const std::string &data) {
        m_client.send_keys(tmux_pane_id, data);
    };

    m_pane_map[tmux_pane_id] = pane;

    // Request current pane content so the display isn't blank
    m_client.send_command(
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

    // Tell tmux our actual window size so it adjusts the session layout.
    // This is critical in PTY mode where we skip the initial refresh-client
    // in initialize() to avoid commands leaking to the shell.
    int w, h;
    m_window.platform()->get_size(w, h);
    int bar_h = m_window.tab_bar_height();
    if (m_cell_w > 0 && m_cell_h > 0) {
        int cols = w / m_cell_w;
        int rows = (h - bar_h) / m_cell_h;
        dbg("tmux: session-changed sending refresh-client %dx%d", cols, rows);
        m_client.refresh_client_size(cols, rows);
    }

    m_client.send_command(
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
            // After layouts are set up, fetch window names and save affinities
            request_window_names();
            save_affinities();
        });
}

void TmuxController::request_window_names() {
    m_client.send_command(
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
                    m_window.platform()->set_title("rivt [tmux] " + pane_title);
                }
            }
        });
}

void TmuxController::save_affinities() {
    // Build an iTerm2-compatible @affinities string so that when iTerm2
    // attaches to this tmux session it groups our windows as tabs rather
    // than opening each as a separate OS window.
    //
    // Decoded format: space-separated equivalence classes, each class is
    //   "wid1,wid2,...;window-options"
    // We put all known windows into one class (rivt uses a single OS window).
    //
    // Encoded format: "a_" prefix + hex-encoded UTF-8 of the decoded string.

    if (m_window_map.empty()) return;

    std::string siblings;
    for (auto &[wid, tab] : m_window_map) {
        if (!siblings.empty()) siblings += ",";
        siblings += std::to_string(wid);
    }
    siblings += ";"; // empty window-options

    // Hex-encode with a_ prefix
    std::string encoded = "a_";
    for (unsigned char c : siblings) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", c);
        encoded += hex;
    }

    std::string cmd = "set @affinities \"" + encoded + "\"";
    if (cmd == m_last_affinities_cmd) return;
    m_last_affinities_cmd = cmd;
    dbg("tmux: save_affinities: %s (decoded: %s)", encoded.c_str(), siblings.c_str());
    m_client.send_command(cmd);
}

void TmuxController::reposition_all_panes() {
    int new_y = m_window.tab_bar_height();
    if (new_y == m_content_y) return;
    dbg("tmux: reposition_all_panes content_y %d -> %d", m_content_y, new_y);
    int delta = new_y - m_content_y;
    m_content_y = new_y;
    for (auto &[wid, tab] : m_window_map) {
        for (auto &p : tab->panes) {
            p->rect.y += delta;
        }
    }
    if (m_tabs.on_needs_render) m_tabs.on_needs_render();
}

void TmuxController::on_session_window_changed(int window_id) {
    dbg("tmux: session-window-changed @%d", window_id);
    auto it = m_window_map.find(window_id);
    if (it == m_window_map.end()) return;
    Tab *tab = it->second;
    for (int i = 0; i < m_tabs.tab_count(); i++) {
        if (m_tabs.tabs()[i].get() == tab) {
            m_tabs.activate_tab(i);
            break;
        }
    }
}

void TmuxController::on_exit() {
    dbg("tmux: on_exit (m_gateway_pane=%p)", (void*)m_gateway_pane);
    m_active = false;

    if (m_gateway_pane) {
        // PTY mode: clean up tmux tabs/panes, restore gateway pane
        // Close all tmux-managed tabs
        for (auto &[wid, tab] : m_window_map) {
            m_tabs.close_tab_ptr(tab);
        }
        m_window_map.clear();
        m_pane_map.clear();
        m_output_buffer.clear();

        // Notify Window to restore normal operation
        if (on_tmux_exit) on_tmux_exit();
    } else {
        // Subprocess mode: close the window
        m_window.mark_closing();
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
