#pragma once
#include "core/types.h"
#include "terminal/vt_parser.h"
#include <deque>
#include <string>
#include <functional>

namespace rivt {

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
    int cols() const { return cols_; }
    int rows() const { return rows_; }

    // Access
    const Line &line(int row) const;    // 0 = top of visible area
    const Cell &cell(int row, int col) const;

    // Scrollback
    int scrollback_count() const { return (int)scrollback_.size(); }
    const Line &scrollback_line(int idx) const;  // 0 = most recent scrollback line
    int viewport_offset() const { return viewport_offset_; }
    void scroll_viewport(int delta);  // negative = scroll up into history
    void scroll_to_bottom();
    bool at_bottom() const { return viewport_offset_ == 0; }

    // Cursor
    int cursor_row() const { return cursor_row_; }
    int cursor_col() const { return cursor_col_; }
    bool cursor_visible() const { return cursor_visible_; }

    // Modes
    bool bracketed_paste() const { return mode_bracketed_paste_; }
    bool app_cursor_keys() const { return mode_app_cursor_; }
    bool focus_reporting() const { return mode_focus_events_; }
    int mouse_mode() const { return mouse_mode_; }
    bool sgr_mouse() const { return mode_sgr_mouse_; }
    bool alt_screen() const { return using_alt_screen_; }

    // Selection
    Selection selection;
    int absolute_line(int screen_row) const {
        return (int)scrollback_.size() + viewport_offset_ + screen_row;
    }
    std::string get_selection_text() const;

    // Kitty keyboard protocol
    // Flags: bit 0 = disambiguate, bit 1 = report event types,
    //        bit 2 = report alternate keys, bit 3 = report all keys as escapes,
    //        bit 4 = report associated text
    int kitty_kbd_flags() const {
        return kitty_kbd_stack_.empty() ? 0 : kitty_kbd_stack_.back();
    }
    bool kitty_kbd_active() const { return kitty_kbd_flags() != 0; }

    // Dirty tracking
    bool any_dirty() const;
    void clear_dirty();

    // Callbacks
    std::function<void(const std::string &)> on_title_change;
    std::function<void()> on_bell;
    std::function<void(const std::string &sel, const std::string &base64)> on_osc52_write;
    std::function<void(const std::string &sel)> on_osc52_read;
    std::function<void(const std::string &)> on_cwd_change;
    std::function<void(const std::string &)> on_write_back;  // send response to PTY

    // VtHandler interface
    void print(uint32_t codepoint) override;
    void execute(uint8_t code) override;
    void csi_dispatch(const CsiParams &params, char intermediate, char final_byte) override;
    void osc_dispatch(int command, const std::string &payload) override;
    void esc_dispatch(char intermediate, char final_byte) override;

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

    void push_scrollback(const Line &line);

    int cols_, rows_;
    int scrollback_limit_;

    // Grid
    std::vector<Line> screen_;       // active screen
    std::vector<Line> alt_screen_;   // alternate screen buffer
    bool using_alt_screen_ = false;

    // Scrollback
    std::deque<Line> scrollback_;
    int viewport_offset_ = 0;

    // Cursor state
    int cursor_row_ = 0;
    int cursor_col_ = 0;
    bool cursor_visible_ = true;

    // Saved cursor (DECSC/DECRC)
    struct SavedCursor {
        int row = 0, col = 0;
        uint32_t fg = COLOR_FLAG_DEFAULT;
        uint32_t bg = COLOR_FLAG_DEFAULT;
        uint16_t attrs = 0;
    };
    SavedCursor saved_cursor_;

    // Current text attributes
    uint32_t cur_fg_ = COLOR_FLAG_DEFAULT;
    uint32_t bg_ = COLOR_FLAG_DEFAULT;
    uint16_t cur_attrs_ = 0;

    // Scroll region
    int scroll_top_ = 0;
    int scroll_bottom_;  // initialized to rows_ - 1

    // Modes
    bool mode_app_cursor_ = false;
    bool mode_cursor_blink_ = false;
    bool mode_focus_events_ = false;
    bool mode_bracketed_paste_ = false;
    int mouse_mode_ = 0;        // 0=off, 1000/1002/1003
    bool mode_sgr_mouse_ = false;
    bool mode_grapheme_cluster_ = false;

    // Kitty keyboard protocol — stack of flag sets
    std::vector<int> kitty_kbd_stack_;
};

} // namespace rivt
