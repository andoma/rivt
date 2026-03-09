#include "terminal/screen_buffer.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace rivt {

ScreenBuffer::ScreenBuffer(int cols, int rows, int scrollback_limit)
    : cols_(cols), rows_(rows), scrollback_limit_(scrollback_limit),
      scroll_bottom_(rows - 1) {
    screen_.resize(rows, Line(cols));
    alt_screen_.resize(rows, Line(cols));
}

void ScreenBuffer::resize(int cols, int rows) {
    // Reflow: for now, simple truncate/pad approach
    // TODO: proper reflow of soft-wrapped lines

    // Push lines that would be lost above the new viewport into scrollback
    if (rows < rows_ && cursor_row_ >= rows) {
        int lines_to_push = cursor_row_ - rows + 1;
        for (int i = 0; i < lines_to_push && !screen_.empty(); i++) {
            push_scrollback(screen_.front());
            screen_.erase(screen_.begin());
        }
        cursor_row_ -= lines_to_push;
    }

    // Resize existing lines
    for (auto &line : screen_) {
        line.resize(cols);
    }

    // Add or remove rows
    while ((int)screen_.size() < rows)
        screen_.emplace_back(cols);
    while ((int)screen_.size() > rows)
        screen_.pop_back();

    // Resize alt screen
    alt_screen_.resize(rows, Line(cols));
    for (auto &line : alt_screen_) {
        line.resize(cols);
    }

    cols_ = cols;
    rows_ = rows;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;

    // Clamp cursor
    cursor_row_ = std::clamp(cursor_row_, 0, rows_ - 1);
    cursor_col_ = std::clamp(cursor_col_, 0, cols_ - 1);

    // Mark all dirty
    for (auto &line : screen_) line.dirty = true;
}

const Line &ScreenBuffer::line(int row) const {
    if (viewport_offset_ != 0) {
        int sb_row = (int)scrollback_.size() + viewport_offset_ + row;
        if (sb_row < 0) {
            static Line empty(0);
            return empty;
        }
        if (sb_row < (int)scrollback_.size()) {
            return scrollback_[sb_row];
        }
        row = sb_row - (int)scrollback_.size();
    }
    if (row >= 0 && row < (int)screen_.size())
        return screen_[row];
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
    int actual = (int)scrollback_.size() - 1 - idx;
    if (actual >= 0 && actual < (int)scrollback_.size())
        return scrollback_[actual];
    static Line empty(0);
    return empty;
}

void ScreenBuffer::scroll_viewport(int delta) {
    int max_offset = -(int)scrollback_.size();
    viewport_offset_ = std::clamp(viewport_offset_ + delta, max_offset, 0);
}

void ScreenBuffer::scroll_to_bottom() {
    viewport_offset_ = 0;
}

bool ScreenBuffer::any_dirty() const {
    for (auto &line : screen_)
        if (line.dirty) return true;
    return false;
}

void ScreenBuffer::clear_dirty() {
    for (auto &line : screen_)
        line.dirty = false;
}

void ScreenBuffer::push_scrollback(const Line &line) {
    if (!using_alt_screen_) {
        scrollback_.push_back(line);
        while ((int)scrollback_.size() > scrollback_limit_)
            scrollback_.pop_front();
    }
}

void ScreenBuffer::put_char(uint32_t cp) {
    if (cursor_col_ >= cols_) {
        // Auto-wrap
        screen_[cursor_row_].cells[cols_ - 1].attrs |= ATTR_WRAP;
        screen_[cursor_row_].wrapped = true;
        new_line();
        cursor_col_ = 0;
    }

    Cell &c = screen_[cursor_row_].cells[cursor_col_];
    c.codepoint = cp;
    c.fg = cur_fg_;
    c.bg = bg_;
    c.attrs = cur_attrs_;
    screen_[cursor_row_].dirty = true;
    cursor_col_++;
}

void ScreenBuffer::new_line() {
    if (cursor_row_ == scroll_bottom_) {
        scroll_up(scroll_top_, scroll_bottom_);
    } else if (cursor_row_ < rows_ - 1) {
        cursor_row_++;
    }
}

void ScreenBuffer::scroll_up(int top, int bottom, int count) {
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; i++) {
        if (top == 0 && !using_alt_screen_) {
            push_scrollback(screen_[top]);

            // Auto-scroll viewport if at bottom
            if (viewport_offset_ < 0)
                viewport_offset_--;
        }
        for (int r = top; r < bottom; r++) {
            screen_[r] = std::move(screen_[r + 1]);
            screen_[r].dirty = true;
        }
        screen_[bottom] = Line(cols_);
        screen_[bottom].dirty = true;
    }
}

void ScreenBuffer::scroll_down(int top, int bottom, int count) {
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; i++) {
        for (int r = bottom; r > top; r--) {
            screen_[r] = std::move(screen_[r - 1]);
            screen_[r].dirty = true;
        }
        screen_[top] = Line(cols_);
        screen_[top].dirty = true;
    }
}

void ScreenBuffer::erase_cells(int row, int start_col, int end_col) {
    if (row < 0 || row >= rows_) return;
    start_col = std::max(start_col, 0);
    end_col = std::min(end_col, cols_ - 1);
    for (int c = start_col; c <= end_col; c++) {
        screen_[row].cells[c].reset();
        screen_[row].cells[c].bg = bg_;
    }
    screen_[row].dirty = true;
}

void ScreenBuffer::erase_line(int row) {
    erase_cells(row, 0, cols_ - 1);
}

void ScreenBuffer::erase_display(int mode) {
    switch (mode) {
        case 0: // Below
            erase_cells(cursor_row_, cursor_col_, cols_ - 1);
            for (int r = cursor_row_ + 1; r < rows_; r++)
                erase_line(r);
            break;
        case 1: // Above
            erase_cells(cursor_row_, 0, cursor_col_);
            for (int r = 0; r < cursor_row_; r++)
                erase_line(r);
            break;
        case 2: // All
            for (int r = 0; r < rows_; r++)
                erase_line(r);
            break;
        case 3: // All + scrollback
            scrollback_.clear();
            for (int r = 0; r < rows_; r++)
                erase_line(r);
            break;
    }
}

void ScreenBuffer::insert_chars(int count) {
    auto &line = screen_[cursor_row_];
    for (int i = cols_ - 1; i >= cursor_col_ + count; i--) {
        line.cells[i] = line.cells[i - count];
    }
    for (int i = cursor_col_; i < std::min(cursor_col_ + count, cols_); i++) {
        line.cells[i].reset();
        line.cells[i].bg = bg_;
    }
    line.dirty = true;
}

void ScreenBuffer::delete_chars(int count) {
    auto &line = screen_[cursor_row_];
    for (int i = cursor_col_; i < cols_ - count; i++) {
        line.cells[i] = line.cells[i + count];
    }
    for (int i = std::max(cols_ - count, cursor_col_); i < cols_; i++) {
        line.cells[i].reset();
        line.cells[i].bg = bg_;
    }
    line.dirty = true;
}

void ScreenBuffer::insert_lines(int count) {
    if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_down(cursor_row_, scroll_bottom_, count);
    }
}

void ScreenBuffer::delete_lines(int count) {
    if (cursor_row_ >= scroll_top_ && cursor_row_ <= scroll_bottom_) {
        scroll_up(cursor_row_, scroll_bottom_, count);
    }
}

void ScreenBuffer::set_cursor(int row, int col) {
    cursor_row_ = std::clamp(row, 0, rows_ - 1);
    cursor_col_ = std::clamp(col, 0, cols_ - 1);
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
            if (cursor_col_ > 0) cursor_col_--;
            break;
        case 0x09: // HT (tab)
            cursor_col_ = std::min(((cursor_col_ / 8) + 1) * 8, cols_ - 1);
            break;
        case 0x0A: // LF
        case 0x0B: // VT
        case 0x0C: // FF
            new_line();
            break;
        case 0x0D: // CR
            cursor_col_ = 0;
            break;
    }
}

void ScreenBuffer::handle_sgr(const CsiParams &params) {
    if (params.count() == 0) {
        cur_fg_ = COLOR_FLAG_DEFAULT;
        bg_ = COLOR_FLAG_DEFAULT;
        cur_attrs_ = 0;
        return;
    }

    for (int i = 0; i < params.count(); i++) {
        int p = params.get(i, 0);

        switch (p) {
            case 0: // Reset
                cur_fg_ = COLOR_FLAG_DEFAULT;
                bg_ = COLOR_FLAG_DEFAULT;
                cur_attrs_ = 0;
                break;
            case 1: cur_attrs_ |= ATTR_BOLD; break;
            case 2: cur_attrs_ |= ATTR_DIM; break;
            case 3: cur_attrs_ |= ATTR_ITALIC; break;
            case 4: {
                // Check for sub-parameters (underline style)
                cur_attrs_ &= ~ATTR_UNDERLINE_MASK;
                if (!params.params[i].sub.empty()) {
                    int style = params.params[i].sub[0];
                    switch (style) {
                        case 0: break; // no underline
                        case 1: cur_attrs_ |= ATTR_UNDERLINE; break;
                        case 2: cur_attrs_ |= ATTR_UNDERLINE_DOUBLE; break;
                        case 3: cur_attrs_ |= ATTR_UNDERLINE_CURLY; break;
                        default: cur_attrs_ |= ATTR_UNDERLINE; break;
                    }
                } else {
                    cur_attrs_ |= ATTR_UNDERLINE;
                }
                break;
            }
            case 7: cur_attrs_ |= ATTR_INVERSE; break;
            case 8: cur_attrs_ |= ATTR_HIDDEN; break;
            case 9: cur_attrs_ |= ATTR_STRIKETHROUGH; break;
            case 21: cur_attrs_ &= ~ATTR_BOLD; break;
            case 22: cur_attrs_ &= ~(ATTR_BOLD | ATTR_DIM); break;
            case 23: cur_attrs_ &= ~ATTR_ITALIC; break;
            case 24: cur_attrs_ &= ~ATTR_UNDERLINE_MASK; break;
            case 27: cur_attrs_ &= ~ATTR_INVERSE; break;
            case 28: cur_attrs_ &= ~ATTR_HIDDEN; break;
            case 29: cur_attrs_ &= ~ATTR_STRIKETHROUGH; break;

            // Foreground colors
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                cur_fg_ = color_palette(p - 30);
                break;
            case 38: // Extended foreground
                if (i + 1 < params.count()) {
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        cur_fg_ = color_palette(params.get(i + 2));
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        cur_fg_ = color_rgb(params.get(i + 2), params.get(i + 3), params.get(i + 4));
                        i += 4;
                    }
                }
                // Also check sub-parameters (colon form: 38:2::R:G:B)
                else if (!params.params[i].sub.empty()) {
                    auto &sub = params.params[i].sub;
                    if (sub.size() >= 1 && sub[0] == 2 && sub.size() >= 4) {
                        // 38:2::R:G:B  (sub[1] may be colorspace, often empty)
                        int r_idx = (sub.size() >= 5) ? 2 : 1;
                        cur_fg_ = color_rgb(
                            std::max(0, sub[r_idx]),
                            std::max(0, sub[r_idx + 1]),
                            std::max(0, sub[r_idx + 2]));
                    } else if (sub.size() >= 1 && sub[0] == 5 && sub.size() >= 2) {
                        cur_fg_ = color_palette(std::max(0, sub[1]));
                    }
                }
                break;
            case 39: cur_fg_ = COLOR_FLAG_DEFAULT; break;

            // Background colors
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                bg_ = color_palette(p - 40);
                break;
            case 48: // Extended background
                if (i + 1 < params.count()) {
                    int mode = params.get(i + 1);
                    if (mode == 5 && i + 2 < params.count()) {
                        bg_ = color_palette(params.get(i + 2));
                        i += 2;
                    } else if (mode == 2 && i + 4 < params.count()) {
                        bg_ = color_rgb(params.get(i + 2), params.get(i + 3), params.get(i + 4));
                        i += 4;
                    }
                }
                else if (!params.params[i].sub.empty()) {
                    auto &sub = params.params[i].sub;
                    if (sub.size() >= 1 && sub[0] == 2 && sub.size() >= 4) {
                        int r_idx = (sub.size() >= 5) ? 2 : 1;
                        bg_ = color_rgb(
                            std::max(0, sub[r_idx]),
                            std::max(0, sub[r_idx + 1]),
                            std::max(0, sub[r_idx + 2]));
                    } else if (sub.size() >= 1 && sub[0] == 5 && sub.size() >= 2) {
                        bg_ = color_palette(std::max(0, sub[1]));
                    }
                }
                break;
            case 49: bg_ = COLOR_FLAG_DEFAULT; break;

            // Bright foreground
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                cur_fg_ = color_palette(p - 90 + 8);
                break;

            // Bright background
            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                bg_ = color_palette(p - 100 + 8);
                break;
        }
    }
}

void ScreenBuffer::set_mode(int mode, bool enable, bool dec_private) {
    if (!dec_private) return;

    switch (mode) {
        case 1:    mode_app_cursor_ = enable; break;
        case 12:   mode_cursor_blink_ = enable; break;
        case 25:   cursor_visible_ = enable; break;
        case 1000: mouse_mode_ = enable ? 1000 : 0; break;
        case 1002: mouse_mode_ = enable ? 1002 : 0; break;
        case 1003: mouse_mode_ = enable ? 1003 : 0; break;
        case 1004: mode_focus_events_ = enable; break;
        case 1006: mode_sgr_mouse_ = enable; break;
        case 1049:
            if (enable) {
                // Save cursor, switch to alt screen, clear
                saved_cursor_ = { cursor_row_, cursor_col_, cur_fg_, bg_, cur_attrs_ };
                if (!using_alt_screen_) {
                    std::swap(screen_, alt_screen_);
                    using_alt_screen_ = true;
                }
                for (auto &line : screen_) {
                    line = Line(cols_);
                }
                set_cursor(0, 0);
            } else {
                // Restore from alt screen
                if (using_alt_screen_) {
                    std::swap(screen_, alt_screen_);
                    using_alt_screen_ = false;
                }
                cursor_row_ = saved_cursor_.row;
                cursor_col_ = saved_cursor_.col;
                cur_fg_ = saved_cursor_.fg;
                bg_ = saved_cursor_.bg;
                cur_attrs_ = saved_cursor_.attrs;
                for (auto &line : screen_) line.dirty = true;
            }
            break;
        case 2004: mode_bracketed_paste_ = enable; break;
        case 2027: mode_grapheme_cluster_ = enable; break;
    }
}

void ScreenBuffer::csi_dispatch(const CsiParams &params, char intermediate, char final_byte) {
    bool dec_private = (intermediate == '?');

    switch (final_byte) {
        case 'A': // CUU - cursor up
            set_cursor(cursor_row_ - std::max(1, params.get(0, 1)), cursor_col_);
            break;
        case 'B': // CUD - cursor down
            set_cursor(cursor_row_ + std::max(1, params.get(0, 1)), cursor_col_);
            break;
        case 'C': // CUF - cursor forward
            set_cursor(cursor_row_, cursor_col_ + std::max(1, params.get(0, 1)));
            break;
        case 'D': // CUB - cursor back
            set_cursor(cursor_row_, cursor_col_ - std::max(1, params.get(0, 1)));
            break;
        case 'E': // CNL - cursor next line
            set_cursor(cursor_row_ + std::max(1, params.get(0, 1)), 0);
            break;
        case 'F': // CPL - cursor preceding line
            set_cursor(cursor_row_ - std::max(1, params.get(0, 1)), 0);
            break;
        case 'G': // CHA - cursor horizontal absolute
            set_cursor(cursor_row_, params.get(0, 1) - 1);
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
                case 0: erase_cells(cursor_row_, cursor_col_, cols_ - 1); break;
                case 1: erase_cells(cursor_row_, 0, cursor_col_); break;
                case 2: erase_line(cursor_row_); break;
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
            scroll_up(scroll_top_, scroll_bottom_, std::max(1, params.get(0, 1)));
            break;
        case 'T': // SD - scroll down
            scroll_down(scroll_top_, scroll_bottom_, std::max(1, params.get(0, 1)));
            break;
        case 'X': // ECH - erase characters
            erase_cells(cursor_row_, cursor_col_,
                        std::min(cursor_col_ + std::max(1, params.get(0, 1)) - 1, cols_ - 1));
            break;
        case '@': // ICH - insert characters
            insert_chars(std::max(1, params.get(0, 1)));
            break;
        case 'd': // VPA - line position absolute
            set_cursor(params.get(0, 1) - 1, cursor_col_);
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
            scroll_top_ = std::max(0, params.get(0, 1) - 1);
            scroll_bottom_ = std::min(rows_ - 1, params.get(1, rows_) - 1);
            set_cursor(0, 0);
            break;
        case 't': // Window manipulation (ignored mostly)
            break;
        case 'n': // DSR - device status report
            if (dec_private && params.get(0) == 6) {
                // CPR: report cursor position
                if (on_write_back) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\033[%d;%dR", cursor_row_ + 1, cursor_col_ + 1);
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
                kitty_kbd_stack_.push_back(flags);
            } else if (intermediate == '<') {
                // Pop N entries from stack
                int n = std::max(1, params.get(0, 1));
                for (int i = 0; i < n && !kitty_kbd_stack_.empty(); i++)
                    kitty_kbd_stack_.pop_back();
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
            auto semi = payload.find(';');
            if (semi != std::string::npos) {
                std::string sel = payload.substr(0, semi);
                std::string data = payload.substr(semi + 1);
                if (data == "?") {
                    if (on_osc52_read) on_osc52_read(sel);
                } else {
                    if (on_osc52_write) on_osc52_write(sel, data);
                }
            }
            break;
        }
        case 133: // Shell integration / semantic zones
            // TODO: track prompt zones
            break;
    }
}

void ScreenBuffer::esc_dispatch(char intermediate, char final_byte) {
    if (intermediate == 0) {
        switch (final_byte) {
            case '7': // DECSC - save cursor
                saved_cursor_ = { cursor_row_, cursor_col_, cur_fg_, bg_, cur_attrs_ };
                break;
            case '8': // DECRC - restore cursor
                cursor_row_ = saved_cursor_.row;
                cursor_col_ = saved_cursor_.col;
                cur_fg_ = saved_cursor_.fg;
                bg_ = saved_cursor_.bg;
                cur_attrs_ = saved_cursor_.attrs;
                break;
            case 'D': // IND - index (move down, scroll if at bottom)
                if (cursor_row_ == scroll_bottom_)
                    scroll_up(scroll_top_, scroll_bottom_);
                else if (cursor_row_ < rows_ - 1)
                    cursor_row_++;
                break;
            case 'E': // NEL - next line
                cursor_col_ = 0;
                if (cursor_row_ == scroll_bottom_)
                    scroll_up(scroll_top_, scroll_bottom_);
                else if (cursor_row_ < rows_ - 1)
                    cursor_row_++;
                break;
            case 'M': // RI - reverse index (move up, scroll if at top)
                if (cursor_row_ == scroll_top_)
                    scroll_down(scroll_top_, scroll_bottom_);
                else if (cursor_row_ > 0)
                    cursor_row_--;
                break;
            case 'c': // RIS - full reset
                *this = ScreenBuffer(cols_, rows_, scrollback_limit_);
                break;
        }
    }
}

static void search_lines(const std::deque<Line> &scrollback,
                         const std::vector<Line> &screen,
                         const std::vector<uint32_t> &qcps,
                         bool case_sensitive, int from, int to,
                         std::vector<SearchMatch> &out) {
    auto tolower_cp = [](uint32_t cp) -> uint32_t {
        return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    };
    int sb_size = (int)scrollback.size();
    for (int ln = from; ln < to; ln++) {
        const Line &line = (ln < sb_size) ? scrollback[ln]
            : ((ln - sb_size < (int)screen.size()) ? screen[ln - sb_size]
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
    search_lines(scrollback_, screen_, qcps, case_sensitive, 0, total, search.matches);
    search.searched_up_to = total;

    // Set current match to the one nearest the viewport bottom
    if (!search.matches.empty()) {
        int bottom_line = (int)scrollback_.size() + viewport_offset_ + rows_ - 1;
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
    // Re-search the active screen area (last rows_ lines) since those
    // lines mutate in place, plus any new scrollback lines.
    int screen_start = (int)scrollback_.size();
    int rescan_from = std::min(search.searched_up_to, screen_start);

    // Remove stale matches from the screen area being rescanned
    while (!search.matches.empty() && search.matches.back().abs_line >= rescan_from)
        search.matches.pop_back();

    search_lines(scrollback_, screen_, qcps, search.case_sensitive,
                 rescan_from, total, search.matches);
    search.searched_up_to = total;
}

std::string ScreenBuffer::get_selection_text() const {
    if (!selection.active) return {};

    int sl, sc, el, ec;
    selection.normalized(sl, sc, el, ec);

    auto get_line = [&](int abs) -> const Line & {
        int sb_size = (int)scrollback_.size();
        if (abs < sb_size) return scrollback_[abs];
        int row = abs - sb_size;
        if (row >= 0 && row < (int)screen_.size()) return screen_[row];
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
