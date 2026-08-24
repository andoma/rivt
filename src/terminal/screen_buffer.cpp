#include "terminal/screen_buffer.h"
#include "terminal/kitty_graphics.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace rivt {

// VT100 DEC Special Graphics character set (maps 0x60..0x7E to Unicode)
static const uint32_t dec_special_graphics[] = {
    0x25C6, // 0x60 ` → ◆ (diamond)
    0x2592, // 0x61 a → ▒ (checkerboard)
    0x2409, // 0x62 b → HT symbol
    0x240C, // 0x63 c → FF symbol
    0x240D, // 0x64 d → CR symbol
    0x240A, // 0x65 e → LF symbol
    0x00B0, // 0x66 f → ° (degree)
    0x00B1, // 0x67 g → ± (plus/minus)
    0x2424, // 0x68 h → NL symbol
    0x240B, // 0x69 i → VT symbol
    0x2518, // 0x6A j → ┘ (lower-right)
    0x2510, // 0x6B k → ┐ (upper-right)
    0x250C, // 0x6C l → ┌ (upper-left)
    0x2514, // 0x6D m → └ (lower-left)
    0x253C, // 0x6E n → ┼ (crossing)
    0x23BA, // 0x6F o → scan line 1
    0x23BB, // 0x70 p → scan line 3
    0x2500, // 0x71 q → ─ (horizontal)
    0x23BC, // 0x72 r → scan line 7
    0x23BD, // 0x73 s → scan line 9
    0x251C, // 0x74 t → ├ (left tee)
    0x2524, // 0x75 u → ┤ (right tee)
    0x2534, // 0x76 v → ┴ (bottom tee)
    0x252C, // 0x77 w → ┬ (top tee)
    0x2502, // 0x78 x → │ (vertical)
    0x2264, // 0x79 y → ≤
    0x2265, // 0x7A z → ≥
    0x03C0, // 0x7B { → π
    0x2260, // 0x7C | → ≠
    0x00A3, // 0x7D } → £
    0x00B7, // 0x7E ~ → · (middle dot)
};

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
    if (cols == m_cols && rows == m_rows) return;
    linearize_screen();

    // Reflow soft-wrapped lines when columns change (not on alt screen)
    if (cols != m_cols && !m_using_alt_screen) {
        reflow(cols, rows);
        return;
    }

    // Rows-only change or alt screen: simple truncate/pad approach

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

    // Pull lines back from scrollback when growing vertically
    while ((int)m_screen.size() < rows && !m_scrollback.empty()) {
        auto line = std::move(m_scrollback.back());
        m_scrollback.pop_back();
        line.resize(cols);
        line.dirty = true;
        m_screen.insert(m_screen.begin(), std::move(line));
        m_cursor_row++;
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

static bool is_default_space(const Cell &c) {
    return c.codepoint == ' ' &&
           (c.fg & COLOR_FLAG_DEFAULT) &&
           (c.bg & COLOR_FLAG_DEFAULT) &&
           c.attrs == 0;
}

void ScreenBuffer::reflow(int new_cols, int new_rows) {
    // Step 1: Save cursor position as (absolute_line_index, col) in the
    // combined scrollback+screen stream.
    int cursor_abs = (int)m_scrollback.size() + m_cursor_row;
    int cursor_col = std::min(m_cursor_col, m_cols - 1);

    // Step 2: Build unified line stream from scrollback + screen.
    std::deque<Line> all_lines;
    for (auto &line : m_scrollback)
        all_lines.push_back(std::move(line));
    for (auto &line : m_screen)
        all_lines.push_back(std::move(line));
    m_scrollback.clear();
    m_screen.clear();
    m_screen_top = 0;

    // Trim trailing empty lines below the cursor to prevent them from
    // pushing content into scrollback after re-wrap.
    while ((int)all_lines.size() > cursor_abs + 1) {
        auto &back = all_lines.back();
        bool all_default = true;
        for (auto &c : back.cells) {
            if (!is_default_space(c)) { all_default = false; break; }
        }
        if (!all_default || back.wrapped) break;
        all_lines.pop_back();
    }

    // Step 3: Group into logical lines.
    // Each logical line is a vector of cells. We track where the cursor falls.
    struct LogicalLine {
        std::vector<Cell> cells;
    };
    std::vector<LogicalLine> logical_lines;
    int cursor_logical = -1;  // which logical line the cursor is on
    int cursor_cell_offset = -1;  // cell offset within that logical line

    int i = 0;
    while (i < (int)all_lines.size()) {
        LogicalLine ll;
        // Gather consecutive wrapped lines into one logical line
        while (i < (int)all_lines.size()) {
            auto &pline = all_lines[i];
            int start_cell = (int)ll.cells.size();

            // Track cursor
            if (i == cursor_abs) {
                cursor_logical = (int)logical_lines.size();
                cursor_cell_offset = start_cell + cursor_col;
            }

            // Append cells from this physical line
            for (auto &c : pline.cells) {
                ll.cells.push_back(c);
            }

            bool was_wrapped = pline.wrapped;
            i++;

            if (!was_wrapped) break;

            // Strip ATTR_WRAP from the last cell of the wrapped line
            if (!ll.cells.empty()) {
                ll.cells[ll.cells.size() - pline.cells.size()  + pline.cells.size() - 1].attrs &= ~ATTR_WRAP;
            }
        }

        // Trim trailing default-space cells from the logical line,
        // but preserve cells up to the cursor position so that
        // trailing spaces before the cursor (e.g. shell prompt "$ ")
        // are not lost.
        int keep = (cursor_logical == (int)logical_lines.size())
                       ? cursor_cell_offset + 1 : 0;
        while ((int)ll.cells.size() > keep && is_default_space(ll.cells.back())) {
            ll.cells.pop_back();
        }

        // Clamp cursor offset if it was on trailing spaces we trimmed
        if (cursor_logical == (int)logical_lines.size() && cursor_cell_offset > (int)ll.cells.size()) {
            cursor_cell_offset = (int)ll.cells.size();
        }

        logical_lines.push_back(std::move(ll));
    }

    // Handle cursor on a line that didn't get processed (shouldn't happen, but be safe)
    if (cursor_logical < 0) {
        cursor_logical = std::max(0, (int)logical_lines.size() - 1);
        cursor_cell_offset = 0;
    }

    // Step 4: Re-wrap logical lines at new width.
    std::deque<Line> new_lines;
    int new_cursor_abs = -1;
    int new_cursor_col = 0;

    for (int li = 0; li < (int)logical_lines.size(); li++) {
        auto &ll = logical_lines[li];

        if (ll.cells.empty()) {
            // Empty logical line → produce one empty physical line
            if (li == cursor_logical) {
                new_cursor_abs = (int)new_lines.size();
                new_cursor_col = 0;
            }
            new_lines.emplace_back(new_cols);
            continue;
        }

        int pos = 0;
        int ncells = (int)ll.cells.size();
        while (pos < ncells) {
            Line pline(new_cols);
            int end = std::min(pos + new_cols, ncells);

            for (int c = pos; c < end; c++) {
                pline.cells[c - pos] = ll.cells[c];
            }

            // Track cursor
            if (li == cursor_logical && cursor_cell_offset >= pos && cursor_cell_offset < pos + new_cols) {
                new_cursor_abs = (int)new_lines.size();
                new_cursor_col = cursor_cell_offset - pos;
            }

            bool more_cells = (end < ncells);
            if (more_cells) {
                // Mark as wrapped continuation
                pline.wrapped = true;
                pline.cells[new_cols - 1].attrs |= ATTR_WRAP;
            }

            pline.dirty = true;
            new_lines.push_back(std::move(pline));
            pos = end;
        }

        // Cursor was past the end of content (e.g. at end of line)
        if (li == cursor_logical && new_cursor_abs < 0) {
            new_cursor_abs = (int)new_lines.size() - 1;
            new_cursor_col = std::min(cursor_cell_offset - (int)(((ncells - 1) / new_cols) * new_cols), new_cols - 1);
            if (new_cursor_col < 0) new_cursor_col = 0;
        }
    }

    if (new_lines.empty()) {
        new_lines.emplace_back(new_cols);
    }

    // Handle edge case: cursor wasn't found
    if (new_cursor_abs < 0) {
        new_cursor_abs = (int)new_lines.size() - 1;
        new_cursor_col = 0;
    }

    // Step 5: Split into scrollback + screen.
    // Last new_rows lines become screen; everything before is scrollback.
    m_screen.clear();
    m_scrollback.clear();

    int total = (int)new_lines.size();
    int screen_start = std::max(0, total - new_rows);

    for (int j = 0; j < screen_start; j++) {
        m_scrollback.push_back(std::move(new_lines[j]));
    }
    for (int j = screen_start; j < total; j++) {
        m_screen.push_back(std::move(new_lines[j]));
    }

    // Pad with empty lines if fewer than new_rows
    while ((int)m_screen.size() < new_rows) {
        m_screen.emplace_back(new_cols);
    }

    // Trim scrollback to limit
    while ((int)m_scrollback.size() > m_scrollback_limit) {
        m_scrollback.pop_front();
        m_scrollback_trimmed++;
    }

    // Step 6: Restore cursor — map new absolute line to screen row.
    m_cursor_row = new_cursor_abs - screen_start;
    m_cursor_col = new_cursor_col;

    // Clamp cursor
    m_cursor_row = std::clamp(m_cursor_row, 0, new_rows - 1);
    m_cursor_col = std::clamp(m_cursor_col, 0, new_cols - 1);

    // Step 7: Update dimensions and invalidate state.
    m_cols = new_cols;
    m_rows = new_rows;
    m_screen_top = 0;
    m_scroll_top = 0;
    m_scroll_bottom = m_rows - 1;

    // Resize alt screen (simple truncate/pad, never reflowed)
    m_alt_screen.resize(new_rows, Line(new_cols));
    for (auto &line : m_alt_screen)
        line.resize(new_cols);

    // Clear selection, search, image placements (abs line refs invalidated)
    selection.clear();
    search.clear();
    images.clear_placements();
    m_viewport_offset = 0;

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
    if (on_scrollback_wanted && m_scrollback_trimmed > 0 &&
        m_viewport_offset <= max_offset + 200)
        on_scrollback_wanted();
}

void ScreenBuffer::prepend_scrollback(std::vector<Line> &&lines) {
    if (lines.empty()) return;
    m_scrollback_trimmed -= (int)lines.size();
    m_scrollback.insert(m_scrollback.begin(),
                        std::make_move_iterator(lines.begin()),
                        std::make_move_iterator(lines.end()));
    // viewport_offset is bottom-relative and absolute indices include
    // m_scrollback_trimmed, so nothing else moves. All prepended lines
    // render dirty if scrolled into view (Line ctor default).
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
            m_scrollback_trimmed++;
            images.gc_placements(m_scrollback_trimmed);
            // If the trimmed line was above the viewport, adjust so the
            // viewport doesn't drift past the start of scrollback.
            if (m_viewport_offset < -(int)m_scrollback.size())
                m_viewport_offset = -(int)m_scrollback.size();
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
    c.attrs = m_cur_attrs | (m_cur_hyperlink_id ? ATTR_HYPERLINK : 0);
    c.hyperlink_id = m_cur_hyperlink_id;
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
            m_viewport_offset = 0;
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
    // Translate through active character set (G0 or G1)
    int charset = (m_gl_charset == 0) ? m_charset_g0 : m_charset_g1;
    if (charset == 1 && codepoint >= 0x60 && codepoint <= 0x7E)
        codepoint = dec_special_graphics[codepoint - 0x60];
    m_last_printed = codepoint;
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
        case 0x0E: // SO - shift out (activate G1)
            m_gl_charset = 1;
            break;
        case 0x0F: // SI - shift in (activate G0)
            m_gl_charset = 0;
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
                if (!params.params[i].sub.empty()) {
                    // Colon form: 38:2::R:G:B or 38:5:N
                    auto &sub = params.params[i].sub;
                    if (sub.size() >= 1 && sub[0] == 2 && sub.size() >= 4) {
                        int r_idx = (sub.size() >= 5) ? 2 : 1;
                        m_cur_fg = color_rgb(
                            std::max(0, sub[r_idx]),
                            std::max(0, sub[r_idx + 1]),
                            std::max(0, sub[r_idx + 2]));
                    } else if (sub.size() >= 1 && sub[0] == 5 && sub.size() >= 2) {
                        m_cur_fg = color_palette(std::max(0, sub[1]));
                    }
                } else if (i + 1 < params.count()) {
                    // Semicolon form: 38;2;R;G;B or 38;5;N
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        m_cur_fg = color_palette(params.get(i + 2));
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        m_cur_fg = color_rgb(params.get(i + 2), params.get(i + 3), params.get(i + 4));
                        i += 4;
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
                if (!params.params[i].sub.empty()) {
                    // Colon form: 48:2::R:G:B or 48:5:N
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
                } else if (i + 1 < params.count()) {
                    // Semicolon form: 48;2;R;G;B or 48;5;N
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        m_bg = color_palette(params.get(i + 2));
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        m_bg = color_rgb(params.get(i + 2), params.get(i + 3), params.get(i + 4));
                        i += 4;
                    }
                }
                break;
            case 49: m_bg = COLOR_FLAG_DEFAULT; break;

            case 58: // Underline color (not stored, but must consume params)
                if (!params.params[i].sub.empty()) {
                    // Colon form: 58:2::R:G:B or 58:5:N — already one param, nothing to skip
                } else if (i + 1 < params.count()) {
                    // Semicolon form: 58;2;R;G;B or 58;5;N — consume extra params
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        i += 4;
                    }
                }
                break;
            case 59: break; // Default underline color (no-op)

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
                selection.clear();
                m_saved_cursor = { m_cursor_row, m_cursor_col, m_cur_fg, m_bg, m_cur_attrs, m_charset_g0, m_charset_g1, m_gl_charset };
                m_saved_kitty_kbd_stack = m_kitty_kbd_stack;
                if (!m_using_alt_screen) {
                    std::swap(m_screen, m_alt_screen);
                    m_using_alt_screen = true;
                    m_screen_top = 0;
                    m_saved_viewport_offset = m_viewport_offset;
                    m_viewport_offset = 0;
                }
                for (auto &line : m_screen) {
                    line = Line(m_cols);
                }
                set_cursor(0, 0);
            } else {
                // Restore from alt screen
                selection.clear();
                if (m_using_alt_screen) {
                    linearize_screen();
                    std::swap(m_screen, m_alt_screen);
                    m_using_alt_screen = false;
                    m_screen_top = 0;
                    m_viewport_offset = m_saved_viewport_offset;
                    m_saved_viewport_offset = 0;
                }
                m_cursor_row = m_saved_cursor.row;
                m_cursor_col = m_saved_cursor.col;
                m_cur_fg = m_saved_cursor.fg;
                m_bg = m_saved_cursor.bg;
                m_cur_attrs = m_saved_cursor.attrs;
                m_charset_g0 = m_saved_cursor.charset_g0;
                m_charset_g1 = m_saved_cursor.charset_g1;
                m_gl_charset = m_saved_cursor.gl_charset;
                m_kitty_kbd_stack = m_saved_kitty_kbd_stack;
                m_saved_kitty_kbd_stack.clear();
                for (auto &line : m_screen) line.dirty = true;
            }
            break;
        case 2004: m_mode_bracketed_paste = enable; break;
        case 2026: m_mode_synchronized_update = enable; break;
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
        case 'b': // REP - repeat preceding graphic character
            if (m_last_printed) {
                int count = std::max(1, params.get(0, 1));
                for (int i = 0; i < count; i++)
                    put_char(m_last_printed);
            }
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
        case 'm': // SGR — only when no intermediate (plain CSI, not CSI > or CSI ?)
            if (intermediate == 0)
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
                // Kitty metadata pairs are always terminated by ';'. A lone '='
                // without a following ';' is base64 padding, not metadata.
                while (!data.empty()) {
                    auto sc = data.find(';');
                    if (sc == std::string::npos) break;
                    auto eq = data.find('=');
                    if (eq == std::string::npos || eq >= sc) break;
                    std::string key = data.substr(0, eq);
                    std::string val = data.substr(eq + 1, sc - eq - 1);
                    if (key == "type") mime_type = val;
                    data = data.substr(sc + 1);
                }

                if (data == "?") {
                    if (on_osc52_read) on_osc52_read(sel, mime_type);
                } else {
                    if (on_osc52_write) on_osc52_write(sel, data, mime_type);
                }
            }
            break;
        }
        case 8: { // Hyperlink: payload is "params;uri"
            auto semi = payload.find(';');
            if (semi == std::string::npos) break;
            std::string uri = payload.substr(semi + 1);
            if (uri.empty()) {
                // End hyperlink
                m_cur_hyperlink_id = 0;
            } else {
                // Start hyperlink — allocate an ID
                m_cur_hyperlink_id = m_next_hyperlink_id++;
                if (m_next_hyperlink_id == 0) m_next_hyperlink_id = 1;  // skip 0
                m_hyperlinks[m_cur_hyperlink_id] = uri;
            }
            break;
        }
        case 133: // Shell integration / semantic zones
            if (!payload.empty()) {
                char zone = payload[0];
                if (zone == 'A' || zone == 'C')
                    sline(m_cursor_row).semantic_zone = zone;
            }
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
                m_saved_cursor = { m_cursor_row, m_cursor_col, m_cur_fg, m_bg, m_cur_attrs, m_charset_g0, m_charset_g1, m_gl_charset };
                break;
            case '8': // DECRC - restore cursor
                m_cursor_row = m_saved_cursor.row;
                m_cursor_col = m_saved_cursor.col;
                m_cur_fg = m_saved_cursor.fg;
                m_bg = m_saved_cursor.bg;
                m_cur_attrs = m_saved_cursor.attrs;
                m_charset_g0 = m_saved_cursor.charset_g0;
                m_charset_g1 = m_saved_cursor.charset_g1;
                m_gl_charset = m_saved_cursor.gl_charset;
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
    } else if (intermediate == '(') {
        // SCS - designate G0 character set
        m_charset_g0 = (final_byte == '0') ? 1 : 0;
    } else if (intermediate == ')') {
        // SCS - designate G1 character set
        m_charset_g1 = (final_byte == '0') ? 1 : 0;
    }
}

static void search_lines(const std::deque<Line> &scrollback,
                         const std::vector<Line> &screen,
                         int screen_top,
                         const std::vector<uint32_t> &qcps,
                         bool case_sensitive, int from, int to,
                         int abs_offset,
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
                out.push_back({abs_offset + ln, col, col + qlen - 1});
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
    search_lines(m_scrollback, m_screen, m_screen_top, qcps, case_sensitive, 0, total,
                 m_scrollback_trimmed, search.matches);
    search.searched_up_to = total;

    // Set current match to the one nearest the viewport bottom
    if (!search.matches.empty()) {
        int bottom_line = absolute_line(m_rows - 1);
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
    while (!search.matches.empty() && search.matches.back().abs_line >= m_scrollback_trimmed + rescan_from)
        search.matches.pop_back();

    search_lines(m_scrollback, m_screen, m_screen_top, qcps, search.case_sensitive,
                 rescan_from, total, m_scrollback_trimmed, search.matches);
    search.searched_up_to = total;
}

std::string ScreenBuffer::get_selection_text() const {
    if (!selection.active) return {};

    int sl, sc, el, ec;
    selection.normalized(sl, sc, el, ec);

    auto get_line = [&](int abs) -> const Line & {
        int idx = abs - m_scrollback_trimmed;
        int sb_size = (int)m_scrollback.size();
        if (idx < 0) { static Line empty(0); return empty; }
        if (idx < sb_size) return m_scrollback[idx];
        int row = idx - sb_size;
        if (row >= 0 && row < (int)m_screen.size()) return sline(row);
        static Line empty(0);
        return empty;
    };

    // For rectangular selection, use fixed column range on every line
    int rect_left = 0, rect_right = 0;
    if (selection.rectangular)
        selection.rect_cols(rect_left, rect_right);

    auto append_utf8 = [](std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    };

    std::string result;
    for (int ln = sl; ln <= el; ln++) {
        const Line &line = get_line(ln);
        int c0, c1;
        if (selection.rectangular) {
            c0 = rect_left;
            c1 = rect_right;
        } else {
            c0 = (ln == sl) ? sc : 0;
            c1 = (ln == el) ? ec : (int)line.cells.size() - 1;
        }

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
            append_utf8(result, line.cells[c].codepoint);
        }

        // Rectangular: always add newlines between lines
        // Normal: add newline except after wrapped lines
        if (ln < el && (selection.rectangular || !line.wrapped)) {
            result += '\n';
        }
    }

    return result;
}

bool ScreenBuffer::find_command_output(int abs_line, int &out_start, int &out_end) const {
    int first_abs = m_scrollback_trimmed;
    int last_abs = m_scrollback_trimmed + (int)m_scrollback.size() + m_rows - 1;

    auto get_zone = [&](int abs) -> uint32_t {
        int idx = abs - m_scrollback_trimmed;
        int sb_size = (int)m_scrollback.size();
        if (idx < 0) return 0;
        if (idx < sb_size) return m_scrollback[idx].semantic_zone;
        int row = idx - sb_size;
        if (row >= 0 && row < m_rows) return sline(row).semantic_zone;
        return 0;
    };

    // If clicked line itself has a marker, it's a prompt line, not output
    if (get_zone(abs_line) != 0) return false;

    // Scan backward to find the nearest 'C' (output start) marker
    bool found_start = false;
    for (int i = abs_line - 1; i >= first_abs; i--) {
        uint32_t z = get_zone(i);
        if (z == 'C') { out_start = i + 1; found_start = true; break; }
        if (z == 'A') return false;  // in prompt area, not output
    }
    if (!found_start) return false;

    // Scan forward to find the next 'A' or 'C' marker (next prompt)
    out_end = last_abs;
    for (int i = abs_line + 1; i <= last_abs; i++) {
        uint32_t z = get_zone(i);
        if (z == 'A' || z == 'C') { out_end = i - 1; break; }
    }

    return out_start <= out_end;
}

std::string ScreenBuffer::detect_url_at(int screen_row, int col) const {
    const Line &l = line(screen_row);
    int ncells = (int)l.cells.size();
    if (col < 0 || col >= ncells) return {};

    // Check for OSC 8 explicit hyperlink first
    const Cell &clicked = l.cells[col];
    if ((clicked.attrs & ATTR_HYPERLINK) && clicked.hyperlink_id) {
        auto it = m_hyperlinks.find(clicked.hyperlink_id);
        if (it != m_hyperlinks.end())
            return it->second;
    }

    // Auto-detect URLs. A URL may wrap across several physical rows, so
    // reconstruct the full logical line (the run of rows joined by the
    // `wrapped` flag) that contains screen_row and scan that instead of a
    // single row. cps holds the concatenated codepoints; click_idx is the
    // clicked cell's index within that logical line.
    int start_row = screen_row;
    for (int guard = 0; guard < 10000 && line(start_row - 1).wrapped; guard++)
        start_row--;

    std::vector<uint32_t> cps;
    int click_idx = -1;
    for (int r = start_row, guard = 0; guard < 10000; r++, guard++) {
        const Line &lr = line(r);
        if (r == screen_row) click_idx = (int)cps.size() + col;
        for (const Cell &cell : lr.cells)
            cps.push_back(cell.codepoint);
        if (!lr.wrapped) break;
    }
    ncells = (int)cps.size();

    auto cp_at = [&](int c) -> uint32_t {
        return (c >= 0 && c < ncells) ? cps[c] : 0;
    };

    auto is_url_char = [](uint32_t cp) -> bool {
        if (cp <= ' ' || cp >= 0x7F) return false;
        if (cp == '<' || cp == '>' || cp == '"' || cp == '\'') return false;
        return true;
    };

    auto match_at = [&](int pos, const char *str) -> bool {
        for (int i = 0; str[i]; i++) {
            if (pos + i >= ncells || cp_at(pos + i) != (uint32_t)(unsigned char)str[i])
                return false;
        }
        return true;
    };

    static const struct { const char *prefix; int len; } schemes[] = {
        {"https://", 8},
        {"http://", 7},
        {"ftp://", 6},
        {"file://", 7},
    };

    for (int pos = 0; pos < ncells; ) {
        int url_start = -1;
        bool prepend_http = false;

        for (auto &s : schemes) {
            if (match_at(pos, s.prefix)) {
                url_start = pos;
                break;
            }
        }
        if (url_start < 0 && match_at(pos, "www.")) {
            url_start = pos;
            prepend_http = true;
        }

        if (url_start < 0) { pos++; continue; }

        // Ensure URL doesn't start mid-word (preceded by alphanumeric)
        if (url_start > 0) {
            uint32_t prev = cp_at(url_start - 1);
            if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                (prev >= '0' && prev <= '9')) {
                pos++;
                continue;
            }
        }

        // Find end of URL
        int url_end = url_start;
        while (url_end < ncells && is_url_char(cp_at(url_end)))
            url_end++;

        // Strip trailing punctuation that's unlikely part of URL
        while (url_end > url_start) {
            uint32_t c = cp_at(url_end - 1);
            if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
                url_end--;
            } else if (c == ')') {
                // Only strip closing paren if unmatched
                int opens = 0, closes = 0;
                for (int i = url_start; i < url_end; i++) {
                    if (cp_at(i) == '(') opens++;
                    if (cp_at(i) == ')') closes++;
                }
                if (closes > opens) url_end--;
                else break;
            } else {
                break;
            }
        }

        // Check if clicked cell is within this URL
        if (click_idx >= url_start && click_idx < url_end) {
            std::string url;
            if (prepend_http) url = "http://";
            for (int c = url_start; c < url_end; c++) {
                uint32_t cp = cp_at(c);
                if (cp < 0x80) url += (char)cp;
            }
            return url;
        }

        pos = url_end;
    }

    return {};
}

} // namespace rivt
