#pragma once
#include "core/types.h"
#include "terminal/vt_parser.h"
#include "terminal/image_store.h"
#include <deque>
#include <string>
#include <vector>
#include <functional>

namespace rivt {

struct SearchMatch {
    int abs_line;
    int start_col;
    int end_col;  // inclusive
};

struct SearchState {
    bool active = false;    // highlights visible
    bool focused = false;   // search bar has keyboard focus
    std::string query;
    std::vector<SearchMatch> matches;
    int current_match = -1;  // index into matches, -1 = none
    bool case_sensitive = false;

    int searched_up_to = 0;  // absolute line count last fully searched

    void clear() {
        active = false;
        focused = false;
        query.clear();
        matches.clear();
        current_match = -1;
        searched_up_to = 0;
    }

    int total_matches() const { return (int)matches.size(); }

    // Check if a cell is part of any match; returns 0=no, 1=match, 2=current match
    int match_type(int abs_line, int col) const {
        for (int i = 0; i < (int)matches.size(); i++) {
            const auto &m = matches[i];
            if (m.abs_line == abs_line && col >= m.start_col && col <= m.end_col) {
                return (i == current_match) ? 2 : 1;
            }
        }
        return 0;
    }
};

struct Selection {
    bool active = false;
    // Absolute line coordinates (scrollback-aware)
    int start_line = 0, start_col = 0;
    int end_line = 0, end_col = 0;

    void clear() { active = false; }

    // Return normalized range (start <= end)
    void normalized(int &sl, int &sc, int &el, int &ec) const {
        if (start_line < end_line || (start_line == end_line && start_col <= end_col)) {
            sl = start_line; sc = start_col; el = end_line; ec = end_col;
        } else {
            sl = end_line; sc = end_col; el = start_line; ec = start_col;
        }
    }

    bool contains(int abs_line, int col) const {
        if (!active) return false;
        int sl, sc, el, ec;
        normalized(sl, sc, el, ec);
        if (abs_line < sl || abs_line > el) return false;
        if (abs_line == sl && abs_line == el) return col >= sc && col <= ec;
        if (abs_line == sl) return col >= sc;
        if (abs_line == el) return col <= ec;
        return true;
    }
};

class ScreenBuffer : public VtHandler {
public:
    ScreenBuffer(int cols, int rows, int scrollback_limit = 10000);

    void resize(int cols, int rows);
    int cols() const { return m_cols; }
    int rows() const { return m_rows; }

    // Access
    const Line &line(int row) const;    // 0 = top of visible area
    const Cell &cell(int row, int col) const;

    // Scrollback
    int scrollback_count() const { return (int)m_scrollback.size(); }
    const Line &scrollback_line(int idx) const;  // 0 = most recent scrollback line
    int viewport_offset() const { return m_viewport_offset; }
    void scroll_viewport(int delta);  // negative = scroll up into history
    void scroll_to_bottom();
    bool at_bottom() const { return m_viewport_offset == 0; }

    // Cursor
    int cursor_row() const { return m_cursor_row; }
    int cursor_col() const { return m_cursor_col; }
    bool cursor_visible() const { return m_cursor_visible; }

    // Modes
    bool bracketed_paste() const { return m_mode_bracketed_paste; }
    bool app_cursor_keys() const { return m_mode_app_cursor; }
    bool focus_reporting() const { return m_mode_focus_events; }
    int mouse_mode() const { return m_mouse_mode; }
    bool sgr_mouse() const { return m_mode_sgr_mouse; }
    bool alt_screen() const { return m_using_alt_screen; }

    // Selection
    Selection selection;
    int absolute_line(int screen_row) const {
        return (int)m_scrollback.size() + m_viewport_offset + screen_row;
    }
    std::string get_selection_text() const;

    // Search
    SearchState search;
    void find_matches(const std::string &query, bool case_sensitive);
    void find_matches_incremental();  // only search new lines since last call
    int total_lines() const { return (int)m_scrollback.size() + m_rows; }

    // Kitty keyboard protocol
    // Flags: bit 0 = disambiguate, bit 1 = report event types,
    //        bit 2 = report alternate keys, bit 3 = report all keys as escapes,
    //        bit 4 = report associated text
    int kitty_kbd_flags() const {
        return m_kitty_kbd_stack.empty() ? 0 : m_kitty_kbd_stack.back();
    }
    bool kitty_kbd_active() const { return kitty_kbd_flags() != 0; }

    // Dirty tracking
    bool any_dirty() const;
    void clear_dirty();

    // Synchronized update (DEC private mode 2026)
    bool synchronized_update() const { return m_mode_synchronized_update; }

    // Callbacks
    std::function<void(const std::string &)> on_title_change;
    std::function<void()> on_bell;
    std::function<void(const std::string &sel, const std::string &base64, const std::string &mime_type)> on_osc52_write;
    std::function<void(const std::string &sel, const std::string &mime_type)> on_osc52_read;
    std::function<void(const std::string &)> on_cwd_change;
    std::function<void(const std::string &)> on_write_back;  // send response to PTY

    // VtHandler interface
    void print(uint32_t codepoint) override;
    void execute(uint8_t code) override;
    void csi_dispatch(const CsiParams &params, char intermediate, char final_byte) override;
    void osc_dispatch(int command, const std::string &payload) override;
    void esc_dispatch(char intermediate, char final_byte) override;
    void apc_dispatch(const std::string &payload) override;

private:
    void put_char(uint32_t cp);
    void new_line();
    void scroll_up(int top, int bottom, int count = 1);
    void scroll_down(int top, int bottom, int count = 1);
    void erase_cells(int row, int start_col, int end_col);
    void erase_line(int row);
    void erase_display(int mode);
    void insert_chars(int count);
    void delete_chars(int count);
    void insert_lines(int count);
    void delete_lines(int count);
    void set_cursor(int row, int col);
    void handle_sgr(const CsiParams &params);
    void set_mode(int mode, bool enable, bool dec_private);

    void push_scrollback(Line &&line);
    void linearize_screen();

    // Ring-buffer access: maps logical row to physical index
    Line& sline(int row) { return m_screen[(m_screen_top + row) % (int)m_screen.size()]; }
    const Line& sline(int row) const { return m_screen[(m_screen_top + row) % (int)m_screen.size()]; }

    int m_cols, m_rows;
    int m_scrollback_limit;

    // Grid
    std::vector<Line> m_screen;       // active screen (ring buffer, m_screen_top is index of row 0)
    int m_screen_top = 0;            // ring buffer offset
    std::vector<Line> m_alt_screen;   // alternate screen buffer
    bool m_using_alt_screen = false;

    // Scrollback
    std::deque<Line> m_scrollback;
    int m_viewport_offset = 0;

    // Cursor state
    int m_cursor_row = 0;
    int m_cursor_col = 0;
    bool m_cursor_visible = true;

    // Saved cursor (DECSC/DECRC)
    struct SavedCursor {
        int row = 0, col = 0;
        uint32_t fg = COLOR_FLAG_DEFAULT;
        uint32_t bg = COLOR_FLAG_DEFAULT;
        uint16_t attrs = 0;
        int charset_g0 = 0, charset_g1 = 0, gl_charset = 0;
    };
    SavedCursor m_saved_cursor;

    // Current text attributes
    uint32_t m_cur_fg = COLOR_FLAG_DEFAULT;
    uint32_t m_bg = COLOR_FLAG_DEFAULT;
    uint16_t m_cur_attrs = 0;

    // Scroll region
    int m_scroll_top = 0;
    int m_scroll_bottom;  // initialized to m_rows - 1

    // Modes
    bool m_mode_app_cursor = false;
    bool m_mode_cursor_blink = false;
    bool m_mode_focus_events = false;
    bool m_mode_bracketed_paste = false;
    int m_mouse_mode = 0;        // 0=off, 1000/1002/1003
    bool m_mode_sgr_mouse = false;
    bool m_mode_grapheme_cluster = false;
    bool m_mode_synchronized_update = false;

    // Character set state (G0/G1 designations, GL pointer)
    // 0 = ASCII (B), 1 = DEC Special Graphics (0)
    int m_charset_g0 = 0;
    int m_charset_g1 = 0;
    int m_gl_charset = 0;  // 0 = G0, 1 = G1

    // Last printed codepoint for REP (CSI b)
    uint32_t m_last_printed = 0;

    // Kitty keyboard protocol — stack of flag sets
    std::vector<int> m_kitty_kbd_stack;
    std::vector<int> m_saved_kitty_kbd_stack;  // saved on alt screen entry

public:
    // Image storage for Kitty graphics protocol
    ImageStore images;
};

} // namespace rivt
