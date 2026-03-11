#include "terminal/screen_buffer.h"
#include "terminal/kitty_graphics.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace rivt {

ScreenBuffer::ScreenBuffer(int cols, int rows, int scrollback_limit)
    : m_cols(cols), m_rows(rows), m_scrollback_limit(scrollback_limit),
      m_scroll_bottom(rows - 1) {
    m_screen.resize(rows, Line(cols));
    m_alt_screen.resize(rows, Line(cols));
}

void ScreenBuffer::linearize_screen() {
    if (m_screen_top == 0) return;
    std::rotate(m_screen.begin(), m_screen.begin() + m_screen_top, m_screen.end());
    m_screen_top = 0;
}

void ScreenBuffer::resize(int cols, int rows) {
    linearize_screen();

    // Reflow: for now, simple truncate/pad approach
    // TODO: proper reflow of soft-wrapped lines

    // Push lines that would be lost above the new viewport into scrollback
    if (rows < m_rows && m_cursor_row >= rows) {
        int lines_to_push = m_cursor_row - rows + 1;
        for (int i = 0; i < lines_to_push && !m_screen.empty(); i++) {
            push_scrollback(std::move(m_screen.front()));
            m_screen.erase(m_screen.begin());
        }
        m_cursor_row -= lines_to_push;
    }

    // Resize existing lines
    for (auto &line : m_screen) {
        line.resize(cols);
    }

    // Add or remove rows
    while ((int)m_screen.size() < rows)
        m_screen.emplace_back(cols);
    while ((int)m_screen.size() > rows)
        m_screen.pop_back();

    // Resize alt screen
    m_alt_screen.resize(rows, Line(cols));
    for (auto &line : m_alt_screen) {
        line.resize(cols);
    }

    m_cols = cols;
    m_rows = rows;
    m_scroll_top = 0;
    m_scroll_bottom = m_rows - 1;

    // Clamp cursor
    m_cursor_row = std::clamp(m_cursor_row, 0, m_rows - 1);
    m_cursor_col = std::clamp(m_cursor_col, 0, m_cols - 1);

    // Mark all dirty
    for (auto &line : m_screen) line.dirty = true;
}

const Line &ScreenBuffer::line(int row) const {
    if (m_viewport_offset != 0) {
        int sb_row = (int)m_scrollback.size() + m_viewport_offset + row;
        if (sb_row < 0) {
            static Line empty(0);
            return empty;
        }
        if (sb_row < (int)m_scrollback.size()) {
            return m_scrollback[sb_row];
        }
        row = sb_row - (int)m_scrollback.size();
    }
    if (row >= 0 && row < (int)m_screen.size())
        return sline(row);
    static Line empty(0);
    return empty;
}

const Cell &ScreenBuffer::cell(int row, int col) const {
    const Line &l = line(row);
    if (col >= 0 && col < (int)l.cells.size())
        return l.cells[col];
    static Cell empty;
    return empty;
}

const Line &ScreenBuffer::scrollback_line(int idx) const {
    int actual = (int)m_scrollback.size() - 1 - idx;
    if (actual >= 0 && actual < (int)m_scrollback.size())
        return m_scrollback[actual];
    static Line empty(0);
    return empty;
}

void ScreenBuffer::scroll_viewport(int delta) {
    int max_offset = -(int)m_scrollback.size();
    m_viewport_offset = std::clamp(m_viewport_offset + delta, max_offset, 0);
}

void ScreenBuffer::scroll_to_bottom() {
    m_viewport_offset = 0;
}

bool ScreenBuffer::any_dirty() const {
    for (auto &line : m_screen)
        if (line.dirty) return true;
    return false;
}

void ScreenBuffer::clear_dirty() {
    for (auto &line : m_screen)
        line.dirty = false;
}

void ScreenBuffer::push_scrollback(Line &&line) {
    if (!m_using_alt_screen) {
        m_scrollback.push_back(std::move(line));
        while ((int)m_scrollback.size() > m_scrollback_limit) {
            m_scrollback.pop_front();
            images.gc_placements((int)m_scrollback.size());
        }
    }
}

void ScreenBuffer::put_char(uint32_t cp) {
    if (m_cursor_col >= m_cols) {
        // Auto-wrap
        Line &wl = sline(m_cursor_row);
        wl.cells[m_cols - 1].attrs |= ATTR_WRAP;
        wl.wrapped = true;
        new_line();
        m_cursor_col = 0;
    }

    Line &cl = sline(m_cursor_row);
    Cell &c = cl.cells[m_cursor_col];
    c.codepoint = cp;
    c.fg = m_cur_fg;
    c.bg = m_bg;
    c.attrs = m_cur_attrs;
    cl.dirty = true;
    m_cursor_col++;
}

void ScreenBuffer::new_line() {
    if (m_cursor_row == m_scroll_bottom) {
        scroll_up(m_scroll_top, m_scroll_bottom);
    } else if (m_cursor_row < m_rows - 1) {
        m_cursor_row++;
    }
}

void ScreenBuffer::scroll_up(int top, int bottom, int count) {
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; i++) {
        if (top == 0 && !m_using_alt_screen) {
            push_scrollback(std::move(sline(0)));
            if (m_viewport_offset < 0)
                m_viewport_offset--;
        }
        if (top == 0 && bottom == m_rows - 1) {
            // Fast path: rotate ring buffer instead of shifting all lines
            sline(0) = Line(m_cols);
            m_screen_top = (m_screen_top + 1) % (int)m_screen.size();
            sline(m_rows - 1).dirty = true;
        } else {
            // Scroll region: shift lines within region
            for (int r = top; r < bottom; r++) {
                sline(r) = std::move(sline(r + 1));
                sline(r).dirty = true;
            }
            sline(bottom) = Line(m_cols);
            sline(bottom).dirty = true;
        }
    }
}

void ScreenBuffer::scroll_down(int top, int bottom, int count) {
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; i++) {
        if (top == 0 && bottom == m_rows - 1) {
            // Fast path: rotate ring buffer backwards
            m_screen_top = (m_screen_top + (int)m_screen.size() - 1) % (int)m_screen.size();
            sline(0) = Line(m_cols);
            sline(0).dirty = true;
        } else {
            for (int r = bottom; r > top; r--) {
                sline(r) = std::move(sline(r - 1));
                sline(r).dirty = true;
            }
            sline(top) = Line(m_cols);
            sline(top).dirty = true;
        }
    }
}

void ScreenBuffer::erase_cells(int row, int start_col, int end_col) {
    if (row < 0 || row >= m_rows) return;
    start_col = std::max(start_col, 0);
    end_col = std::min(end_col, m_cols - 1);
    Line &l = sline(row);
    for (int c = start_col; c <= end_col; c++) {
        l.cells[c].reset();
        l.cells[c].bg = m_bg;
    }
    l.dirty = true;
}

void ScreenBuffer::erase_line(int row) {
    erase_cells(row, 0, m_cols - 1);
}

void ScreenBuffer::erase_display(int mode) {
    switch (mode) {
        case 0: // Below
            erase_cells(m_cursor_row, m_cursor_col, m_cols - 1);
            for (int r = m_cursor_row + 1; r < m_rows; r++)
                erase_line(r);
            break;
        case 1: // Above
            erase_cells(m_cursor_row, 0, m_cursor_col);
            for (int r = 0; r < m_cursor_row; r++)
                erase_line(r);
            break;
        case 2: // All
            for (int r = 0; r < m_rows; r++)
                erase_line(r);
            break;
        case 3: // All + scrollback
            m_scrollback.clear();
            images.remove_all();
            for (int r = 0; r < m_rows; r++)
                erase_line(r);
            break;
    }
}

void ScreenBuffer::insert_chars(int count) {
    auto &line = sline(m_cursor_row);
    for (int i = m_cols - 1; i >= m_cursor_col + count; i--) {
        line.cells[i] = line.cells[i - count];
    }
    for (int i = m_cursor_col; i < std::min(m_cursor_col + count, m_cols); i++) {
        line.cells[i].reset();
        line.cells[i].bg = m_bg;
    }
    line.dirty = true;
}

void ScreenBuffer::delete_chars(int count) {
    auto &line = sline(m_cursor_row);
    for (int i = m_cursor_col; i < m_cols - count; i++) {
        line.cells[i] = line.cells[i + count];
    }
    for (int i = std::max(m_cols - count, m_cursor_col); i < m_cols; i++) {
        line.cells[i].reset();
        line.cells[i].bg = m_bg;
    }
    line.dirty = true;
}

void ScreenBuffer::insert_lines(int count) {
    if (m_cursor_row >= m_scroll_top && m_cursor_row <= m_scroll_bottom) {
        scroll_down(m_cursor_row, m_scroll_bottom, count);
    }
}

void ScreenBuffer::delete_lines(int count) {
    if (m_cursor_row >= m_scroll_top && m_cursor_row <= m_scroll_bottom) {
        scroll_up(m_cursor_row, m_scroll_bottom, count);
    }
}

void ScreenBuffer::set_cursor(int row, int col) {
    m_cursor_row = std::clamp(row, 0, m_rows - 1);
    m_cursor_col = std::clamp(col, 0, m_cols - 1);
}

// VtHandler implementation

void ScreenBuffer::print(uint32_t codepoint) {
    put_char(codepoint);
}

void ScreenBuffer::execute(uint8_t code) {
    switch (code) {
        case 0x07: // BEL
            if (on_bell) on_bell();
            break;
        case 0x08: // BS
            if (m_cursor_col > 0) m_cursor_col--;
            break;
        case 0x09: // HT (tab)
            m_cursor_col = std::min(((m_cursor_col / 8) + 1) * 8, m_cols - 1);
            break;
        case 0x0A: // LF
        case 0x0B: // VT
        case 0x0C: // FF
            new_line();
            break;
        case 0x0D: // CR
            m_cursor_col = 0;
            break;
    }
}

void ScreenBuffer::handle_sgr(const CsiParams &params) {
    if (params.count() == 0) {
        m_cur_fg = COLOR_FLAG_DEFAULT;
        m_bg = COLOR_FLAG_DEFAULT;
        m_cur_attrs = 0;
        return;
    }

    for (int i = 0; i < params.count(); i++) {
        int p = params.get(i, 0);

        switch (p) {
            case 0: // Reset
                m_cur_fg = COLOR_FLAG_DEFAULT;
                m_bg = COLOR_FLAG_DEFAULT;
                m_cur_attrs = 0;
                break;
            case 1: m_cur_attrs |= ATTR_BOLD; break;
            case 2: m_cur_attrs |= ATTR_DIM; break;
            case 3: m_cur_attrs |= ATTR_ITALIC; break;
            case 4: {
                // Check for sub-parameters (underline style)
                m_cur_attrs &= ~ATTR_UNDERLINE_MASK;
                if (!params.params[i].sub.empty()) {
                    int style = params.params[i].sub[0];
                    switch (style) {
                        case 0: break; // no underline
                        case 1: m_cur_attrs |= ATTR_UNDERLINE; break;
                        case 2: m_cur_attrs |= ATTR_UNDERLINE_DOUBLE; break;
                        case 3: m_cur_attrs |= ATTR_UNDERLINE_CURLY; break;
                        default: m_cur_attrs |= ATTR_UNDERLINE; break;
                    }
                } else {
                    m_cur_attrs |= ATTR_UNDERLINE;
                }
                break;
            }
            case 7: m_cur_attrs |= ATTR_INVERSE; break;
            case 8: m_cur_attrs |= ATTR_HIDDEN; break;
            case 9: m_cur_attrs |= ATTR_STRIKETHROUGH; break;
            case 21: m_cur_attrs &= ~ATTR_BOLD; break;
            case 22: m_cur_attrs &= ~(ATTR_BOLD | ATTR_DIM); break;
            case 23: m_cur_attrs &= ~ATTR_ITALIC; break;
            case 24: m_cur_attrs &= ~ATTR_UNDERLINE_MASK; break;
            case 27: m_cur_attrs &= ~ATTR_INVERSE; break;
            case 28: m_cur_attrs &= ~ATTR_HIDDEN; break;
            case 29: m_cur_attrs &= ~ATTR_STRIKETHROUGH; break;

            // Foreground colors
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                m_cur_fg = color_palette(p - 30);
                break;
            case 38: // Extended foreground
                if (i + 1 < params.count()) {
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        m_cur_fg = color_palette(params.get(i + 2));
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        m_cur_fg = color_rgb(params.get(i + 2), params.get(i + 3), params.get(i + 4));
                        i += 4;
                    }
                }
                // Also check sub-parameters (colon form: 38:2::R:G:B)
                else if (!params.params[i].sub.empty()) {
                    auto &sub = params.params[i].sub;
                    if (sub.size() >= 1 && sub[0] == 2 && sub.size() >= 4) {
                        // 38:2::R:G:B  (sub[1] may be colorspace, often empty)
                        int r_idx = (sub.size() >= 5) ? 2 : 1;
                        m_cur_fg = color_rgb(
                            std::max(0, sub[r_idx]),
                            std::max(0, sub[r_idx + 1]),
                            std::max(0, sub[r_idx + 2]));
                    } else if (sub.size() >= 1 && sub[0] == 5 && sub.size() >= 2) {
                        m_cur_fg = color_palette(std::max(0, sub[1]));
                    }
                }
                break;
            case 39: m_cur_fg = COLOR_FLAG_DEFAULT; break;

            // Background colors
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                m_bg = color_palette(p - 40);
                break;
            case 48: // Extended background
                if (i + 1 < params.count()) {
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        m_bg = color_palette(params.get(i + 2));
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        m_bg = color_rgb(params.get(i + 2), params.get(i + 3), params.get(i + 4));
                        i += 4;
                    }
                }
                else if (!params.params[i].sub.empty()) {
                    auto &sub = params.params[i].sub;
                    if (sub.size() >= 1 && sub[0] == 2 && sub.size() >= 4) {
                        int r_idx = (sub.size() >= 5) ? 2 : 1;
                        m_bg = color_rgb(
                            std::max(0, sub[r_idx]),
                            std::max(0, sub[r_idx + 1]),
                            std::max(0, sub[r_idx + 2]));
                    } else if (sub.size() >= 1 && sub[0] == 5 && sub.size() >= 2) {
                        m_bg = color_palette(std::max(0, sub[1]));
                    }
                }
                break;
            case 49: m_bg = COLOR_FLAG_DEFAULT; break;

            // Bright foreground
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                m_cur_fg = color_palette(p - 90 + 8);
                break;

            // Bright background
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                m_bg = color_palette(p - 100 + 8);
                break;
        }
    }
}

void ScreenBuffer::set_mode(int mode, bool enable, bool dec_private) {
    if (!dec_private) return;

    switch (mode) {
        case 1:    m_mode_app_cursor = enable; break;
        case 12:   m_mode_cursor_blink = enable; break;
        case 25:   m_cursor_visible = enable; break;
        case 1000: m_mouse_mode = enable ? 1000 : 0; break;
        case 1002: m_mouse_mode = enable ? 1002 : 0; break;
        case 1003: m_mouse_mode = enable ? 1003 : 0; break;
        case 1004: m_mode_focus_events = enable; break;
        case 1006: m_mode_sgr_mouse = enable; break;
        case 1049:
            if (enable) {
                // Save cursor, switch to alt screen, clear
                linearize_screen();
                m_saved_cursor = { m_cursor_row, m_cursor_col, m_cur_fg, m_bg, m_cur_attrs };
                m_saved_kitty_kbd_stack = m_kitty_kbd_stack;
                if (!m_using_alt_screen) {
                    std::swap(m_screen, m_alt_screen);
                    m_using_alt_screen = true;
                    m_screen_top = 0;
                }
                for (auto &line : m_screen) {
                    line = Line(m_cols);
                }
                set_cursor(0, 0);
            } else {
                // Restore from alt screen
                if (m_using_alt_screen) {
                    linearize_screen();
                    std::swap(m_screen, m_alt_screen);
                    m_using_alt_screen = false;
                    m_screen_top = 0;
                }
                m_cursor_row = m_saved_cursor.row;
                m_cursor_col = m_saved_cursor.col;
                m_cur_fg = m_saved_cursor.fg;
                m_bg = m_saved_cursor.bg;
                m_cur_attrs = m_saved_cursor.attrs;
                m_kitty_kbd_stack = m_saved_kitty_kbd_stack;
                m_saved_kitty_kbd_stack.clear();
                for (auto &line : m_screen) line.dirty = true;
            }
            break;
        case 2004: m_mode_bracketed_paste = enable; break;
        case 2027: m_mode_grapheme_cluster = enable; break;
    }
}

void ScreenBuffer::csi_dispatch(const CsiParams &params, char intermediate, char final_byte) {
    bool dec_private = (intermediate == '?');

    switch (final_byte) {
        case 'A': // CUU - cursor up
            set_cursor(m_cursor_row - std::max(1, params.get(0, 1)), m_cursor_col);
            break;
        case 'B': // CUD - cursor down
            set_cursor(m_cursor_row + std::max(1, params.get(0, 1)), m_cursor_col);
            break;
        case 'C': // CUF - cursor forward
            set_cursor(m_cursor_row, m_cursor_col + std::max(1, params.get(0, 1)));
            break;
        case 'D': // CUB - cursor back
            set_cursor(m_cursor_row, m_cursor_col - std::max(1, params.get(0, 1)));
            break;
        case 'E': // CNL - cursor next line
            set_cursor(m_cursor_row + std::max(1, params.get(0, 1)), 0);
            break;
        case 'F': // CPL - cursor preceding line
            set_cursor(m_cursor_row - std::max(1, params.get(0, 1)), 0);
            break;
        case 'G': // CHA - cursor horizontal absolute
            set_cursor(m_cursor_row, params.get(0, 1) - 1);
            break;
        case 'H': // CUP - cursor position
        case 'f': // HVP
            set_cursor(params.get(0, 1) - 1, params.get(1, 1) - 1);
            break;
        case 'J': // ED - erase in display
            erase_display(params.get(0, 0));
            break;
        case 'K': // EL - erase in line
            switch (params.get(0, 0)) {
                case 0: erase_cells(m_cursor_row, m_cursor_col, m_cols - 1); break;
                case 1: erase_cells(m_cursor_row, 0, m_cursor_col); break;
                case 2: erase_line(m_cursor_row); break;
            }
            break;
        case 'L': // IL - insert lines
            insert_lines(std::max(1, params.get(0, 1)));
            break;
        case 'M': // DL - delete lines
            delete_lines(std::max(1, params.get(0, 1)));
            break;
        case 'P': // DCH - delete characters
            delete_chars(std::max(1, params.get(0, 1)));
            break;
        case 'S': // SU - scroll up
            scroll_up(m_scroll_top, m_scroll_bottom, std::max(1, params.get(0, 1)));
            break;
        case 'T': // SD - scroll down
            scroll_down(m_scroll_top, m_scroll_bottom, std::max(1, params.get(0, 1)));
            break;
        case 'X': // ECH - erase characters
            erase_cells(m_cursor_row, m_cursor_col,
                        std::min(m_cursor_col + std::max(1, params.get(0, 1)) - 1, m_cols - 1));
            break;
        case '@': // ICH - insert characters
            insert_chars(std::max(1, params.get(0, 1)));
            break;
        case 'd': // VPA - line position absolute
            set_cursor(params.get(0, 1) - 1, m_cursor_col);
            break;
        case 'h': // SM - set mode
            for (int i = 0; i < params.count(); i++)
                set_mode(params.get(i), true, dec_private);
            break;
        case 'l': // RM - reset mode
            for (int i = 0; i < params.count(); i++)
                set_mode(params.get(i), false, dec_private);
            break;
        case 'm': // SGR
            handle_sgr(params);
            break;
        case 'r': // DECSTBM - set scrolling region
            m_scroll_top = std::max(0, params.get(0, 1) - 1);
            m_scroll_bottom = std::min(m_rows - 1, params.get(1, m_rows) - 1);
            set_cursor(0, 0);
            break;
        case 't': // Window manipulation (ignored mostly)
            break;
        case 'n': // DSR - device status report
            if (dec_private && params.get(0) == 6) {
                // CPR: report cursor position
                if (on_write_back) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\033[%d;%dR", m_cursor_row + 1, m_cursor_col + 1);
                    on_write_back(buf);
                }
            }
            break;
        case 'c': // DA - device attributes
            if (intermediate == '>' && on_write_back) {
                // DA2: report terminal type. Claim VT220-ish.
                on_write_back("\033[>0;0;0c");
            } else if (intermediate == 0 && on_write_back) {
                // DA1: report basic attributes
                on_write_back("\033[?62;22c");
            }
            break;
        case 'u': // Kitty keyboard protocol
            if (intermediate == '?') {
                // Query: report current flags
                if (on_write_back) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "\033[?%du", kitty_kbd_flags());
                    on_write_back(buf);
                }
            } else if (intermediate == '>') {
                // Push flags onto stack
                int flags = params.get(0, 0);
                m_kitty_kbd_stack.push_back(flags);
            } else if (intermediate == '<') {
                // Pop N entries from stack
                int n = std::max(1, params.get(0, 1));
                for (int i = 0; i < n && !m_kitty_kbd_stack.empty(); i++)
                    m_kitty_kbd_stack.pop_back();
            }
            break;
    }
}

void ScreenBuffer::osc_dispatch(int command, const std::string &payload) {
    switch (command) {
        case 0: // Set icon name and window title
        case 2: // Set window title
            if (on_title_change) on_title_change(payload);
            break;
        case 1: // Set icon name (ignore separately)
            break;
        case 7: // Current working directory
            if (on_cwd_change) on_cwd_change(payload);
            break;
        case 52: { // Clipboard: payload is "Pc;Pd" where Pc=selection, Pd=base64 or ?
            // Kitty extended: "Pc;key=val;...base64data" with metadata before payload
            auto semi = payload.find(';');
            if (semi != std::string::npos) {
                std::string sel = payload.substr(0, semi);
                std::string data = payload.substr(semi + 1);

                // Parse optional metadata key=value pairs (Kitty clipboard protocol)
                std::string mime_type;
                while (!data.empty()) {
                    auto eq = data.find('=');
                    auto sc = data.find(';');
                    if (eq != std::string::npos && (sc == std::string::npos || eq < sc)) {
                        // Found a key=value pair before any semicolon
                        std::string segment = data.substr(0, sc != std::string::npos ? sc : data.size());
                        std::string key = segment.substr(0, eq);
                        std::string val = segment.substr(eq + 1);
                        if (key == "type") mime_type = val;
                        if (sc != std::string::npos)
                            data = data.substr(sc + 1);
                        else
                            data.clear();
                    } else {
                        break;  // No more metadata, rest is payload
                    }
                }

                if (data == "?") {
                    if (on_osc52_read) on_osc52_read(sel, mime_type);
                } else {
                    if (on_osc52_write) on_osc52_write(sel, data, mime_type);
                }
            }
            break;
        }
        case 133: // Shell integration / semantic zones
            // TODO: track prompt zones
            break;
    }
}

void ScreenBuffer::apc_dispatch(const std::string &payload) {
    if (payload.empty() || payload[0] != 'G') return;

    auto cmd = parse_kitty_graphics(payload.substr(1));

    auto send_response = [&](const std::string &msg, bool ok) {
        if (cmd.quiet >= 2) return;
        if (cmd.quiet >= 1 && ok) return;
        if (!on_write_back) return;
        std::string resp = "\033_G";
        if (cmd.image_id) resp += "i=" + std::to_string(cmd.image_id) + ",";
        if (cmd.placement_id) resp += "p=" + std::to_string(cmd.placement_id) + ",";
        resp += ok ? "OK" : msg;
        resp += "\033\\";
        on_write_back(resp);
    };

    switch (cmd.action) {
        case 't':  // transmit only
        case 'T': { // transmit + display
            if (cmd.more) {
                images.begin_transfer(cmd);
                images.append_data(cmd.payload);
            } else {
                if (images.has_pending_transfer()) {
                    images.append_data(cmd.payload);
                    auto finished_cmd = images.finish_transfer();
                    finished_cmd.action = cmd.action;
                    if (finished_cmd.action == 'T') {
                        int abs = absolute_line(m_cursor_row);
                        images.place_image(finished_cmd.image_id, finished_cmd.placement_id,
                                          abs, m_cursor_col, finished_cmd.columns, finished_cmd.rows,
                                          finished_cmd.z_index);
                    }
                    send_response("OK", true);
                } else {
                    images.store_image(cmd);
                    if (cmd.action == 'T') {
                        int abs = absolute_line(m_cursor_row);
                        images.place_image(cmd.image_id, cmd.placement_id,
                                          abs, m_cursor_col, cmd.columns, cmd.rows, cmd.z_index);
                    }
                    send_response("OK", true);
                }
            }
            break;
        }
        case 'p': { // place
            int abs = absolute_line(m_cursor_row);
            images.place_image(cmd.image_id, cmd.placement_id,
                              abs, m_cursor_col, cmd.columns, cmd.rows, cmd.z_index);
            send_response("OK", true);
            break;
        }
        case 'd': { // delete
            images.delete_images(cmd);
            send_response("OK", true);
            break;
        }
        case 'q': { // query
            // Just respond OK to queries about supported formats
            send_response("OK", true);
            break;
        }
        default:
            break;
    }
}

void ScreenBuffer::esc_dispatch(char intermediate, char final_byte) {
    if (intermediate == 0) {
        switch (final_byte) {
            case '7': // DECSC - save cursor
                m_saved_cursor = { m_cursor_row, m_cursor_col, m_cur_fg, m_bg, m_cur_attrs };
                break;
            case '8': // DECRC - restore cursor
                m_cursor_row = m_saved_cursor.row;
                m_cursor_col = m_saved_cursor.col;
                m_cur_fg = m_saved_cursor.fg;
                m_bg = m_saved_cursor.bg;
                m_cur_attrs = m_saved_cursor.attrs;
                break;
            case 'D': // IND - index (move down, scroll if at bottom)
                if (m_cursor_row == m_scroll_bottom)
                    scroll_up(m_scroll_top, m_scroll_bottom);
                else if (m_cursor_row < m_rows - 1)
                    m_cursor_row++;
                break;
            case 'E': // NEL - next line
                m_cursor_col = 0;
                if (m_cursor_row == m_scroll_bottom)
                    scroll_up(m_scroll_top, m_scroll_bottom);
                else if (m_cursor_row < m_rows - 1)
                    m_cursor_row++;
                break;
            case 'M': // RI - reverse index (move up, scroll if at top)
                if (m_cursor_row == m_scroll_top)
                    scroll_down(m_scroll_top, m_scroll_bottom);
                else if (m_cursor_row > 0)
                    m_cursor_row--;
                break;
            case 'c': // RIS - full reset
                *this = ScreenBuffer(m_cols, m_rows, m_scrollback_limit);
                break;
        }
    }
}

static void search_lines(const std::deque<Line> &scrollback,
                         const std::vector<Line> &screen,
                         int screen_top,
                         const std::vector<uint32_t> &qcps,
                         bool case_sensitive, int from, int to,
                         std::vector<SearchMatch> &out) {
    auto tolower_cp = [](uint32_t cp) -> uint32_t {
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    };
    int sb_size = (int)scrollback.size();
    int screen_size = (int)screen.size();
    for (int ln = from; ln < to; ln++) {
        const Line &line = (ln < sb_size) ? scrollback[ln]
            : ((ln - sb_size < screen_size) ? screen[(screen_top + ln - sb_size) % screen_size]
               : *(const Line *)nullptr);
        int ncols = (int)line.cells.size();
        int qlen = (int)qcps.size();
        for (int col = 0; col <= ncols - qlen; col++) {
            bool match = true;
            for (int k = 0; k < qlen; k++) {
                uint32_t cp = line.cells[col + k].codepoint;
                if (!case_sensitive) cp = tolower_cp(cp);
                if (cp != qcps[k]) { match = false; break; }
            }
            if (match) {
                out.push_back({ln, col, col + qlen - 1});
            }
        }
    }
}

static std::vector<uint32_t> build_query_cps(const std::string &query, bool case_sensitive) {
    auto tolower_cp = [](uint32_t cp) -> uint32_t {
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    };
    std::vector<uint32_t> qcps;
    for (unsigned char c : query)
        qcps.push_back(case_sensitive ? (uint32_t)c : tolower_cp(c));
    return qcps;
}

void ScreenBuffer::find_matches(const std::string &query, bool case_sensitive) {
    search.matches.clear();
    search.current_match = -1;
    search.searched_up_to = 0;
    if (query.empty()) return;

    auto qcps = build_query_cps(query, case_sensitive);
    int total = total_lines();
    search_lines(m_scrollback, m_screen, m_screen_top, qcps, case_sensitive, 0, total, search.matches);
    search.searched_up_to = total;

    // Set current match to the one nearest the viewport bottom
    if (!search.matches.empty()) {
        int bottom_line = (int)m_scrollback.size() + m_viewport_offset + m_rows - 1;
        search.current_match = 0;
        for (int i = 0; i < (int)search.matches.size(); i++) {
            if (search.matches[i].abs_line <= bottom_line)
                search.current_match = i;
        }
    }
}

void ScreenBuffer::find_matches_incremental() {
    if (search.query.empty()) return;
    int total = total_lines();
    if (total <= search.searched_up_to) return;

    auto qcps = build_query_cps(search.query, search.case_sensitive);
    // Re-search the active screen area (last m_rows lines) since those
    // lines mutate in place, plus any new scrollback lines.
    int screen_start = (int)m_scrollback.size();
    int rescan_from = std::min(search.searched_up_to, screen_start);

    // Remove stale matches from the screen area being rescanned
    while (!search.matches.empty() && search.matches.back().abs_line >= rescan_from)
        search.matches.pop_back();

    search_lines(m_scrollback, m_screen, m_screen_top, qcps, search.case_sensitive,
                 rescan_from, total, search.matches);
    search.searched_up_to = total;
}

std::string ScreenBuffer::get_selection_text() const {
    if (!selection.active) return {};

    int sl, sc, el, ec;
    selection.normalized(sl, sc, el, ec);

    auto get_line = [&](int abs) -> const Line & {
        int sb_size = (int)m_scrollback.size();
        if (abs < sb_size) return m_scrollback[abs];
        int row = abs - sb_size;
        if (row >= 0 && row < (int)m_screen.size()) return sline(row);
        static Line empty(0);
        return empty;
    };

    std::string result;
    for (int ln = sl; ln <= el; ln++) {
        const Line &line = get_line(ln);
        int c0 = (ln == sl) ? sc : 0;
        int c1 = (ln == el) ? ec : (int)line.cells.size() - 1;

        // Find last non-space to trim trailing whitespace
        int last_non_space = c0 - 1;
        for (int c = c1; c >= c0; c--) {
            if (c < (int)line.cells.size() && line.cells[c].codepoint != ' ') {
                last_non_space = c;
                break;
            }
        }

        for (int c = c0; c <= last_non_space && c < (int)line.cells.size(); c++) {
            if (line.cells[c].attrs & ATTR_WIDE_CONT) continue;
            uint32_t cp = line.cells[c].codepoint;
            // UTF-8 encode
            if (cp < 0x80) {
                result += (char)cp;
            } else if (cp < 0x800) {
                result += (char)(0xC0 | (cp >> 6));
                result += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                result += (char)(0xE0 | (cp >> 12));
                result += (char)(0x80 | ((cp >> 6) & 0x3F));
                result += (char)(0x80 | (cp & 0x3F));
            } else {
                result += (char)(0xF0 | (cp >> 18));
                result += (char)(0x80 | ((cp >> 12) & 0x3F));
                result += (char)(0x80 | ((cp >> 6) & 0x3F));
                result += (char)(0x80 | (cp & 0x3F));
            }
        }

        // Add newline between lines (not after wrapped lines within selection)
        if (ln < el && !line.wrapped) {
            result += '\n';
        }
    }

    return result;
}

} // namespace rivt
