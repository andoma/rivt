#include "test.h"
#include "terminal/vt_parser.h"
#include "terminal/screen_buffer.h"

using namespace rivt;

// Helper: create a screen buffer + parser, feed a string, return the buffer
struct TestTerminal {
    ScreenBuffer screen;
    VtParser parser;
    std::string last_title;

    TestTerminal(int cols = 80, int rows = 24)
        : screen(cols, rows, 1000), parser(screen)
    {
        screen.on_title_change = [this](const std::string &t) { last_title = t; };
    }

    void feed(const char *s) { parser.feed(s, strlen(s)); }

    // Read a line as a string (trimming trailing spaces)
    std::string line_text(int row) {
        const Line &l = screen.line(row);
        std::string s;
        for (auto &c : l.cells) {
            if (c.codepoint < 128)
                s += (char)c.codepoint;
            else
                s += '?';
        }
        // Trim trailing spaces
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        return s;
    }
};

// --- Basic printing ---

TEST(screen_print_hello) {
    TestTerminal t;
    t.feed("Hello");
    ASSERT_STR_EQ(t.line_text(0), "Hello");
    ASSERT_EQ(t.screen.cursor_col(), 5);
    ASSERT_EQ(t.screen.cursor_row(), 0);
}

TEST(screen_print_newline) {
    TestTerminal t;
    t.feed("Line1\r\nLine2");
    ASSERT_STR_EQ(t.line_text(0), "Line1");
    ASSERT_STR_EQ(t.line_text(1), "Line2");
    ASSERT_EQ(t.screen.cursor_row(), 1);
}

// --- Cursor movement ---

TEST(screen_cursor_up) {
    TestTerminal t;
    t.feed("A\r\nB\r\nC\033[2A");  // move up 2
    ASSERT_EQ(t.screen.cursor_row(), 0);
}

TEST(screen_cursor_position) {
    TestTerminal t;
    t.feed("\033[5;10H");  // row 5, col 10 (1-based)
    ASSERT_EQ(t.screen.cursor_row(), 4);
    ASSERT_EQ(t.screen.cursor_col(), 9);
}

TEST(screen_cursor_position_default) {
    TestTerminal t;
    t.feed("\033[5;10H");
    t.feed("\033[H");  // should go to 1,1
    ASSERT_EQ(t.screen.cursor_row(), 0);
    ASSERT_EQ(t.screen.cursor_col(), 0);
}

TEST(screen_cursor_clamp) {
    TestTerminal t(10, 5);
    t.feed("\033[999;999H");
    ASSERT_EQ(t.screen.cursor_row(), 4);
    ASSERT_EQ(t.screen.cursor_col(), 9);
}

// --- Erase ---

TEST(screen_erase_display_all) {
    TestTerminal t(10, 5);
    t.feed("ABCDE\r\nFGHIJ");
    t.feed("\033[2J");  // erase all
    ASSERT_STR_EQ(t.line_text(0), "");
    ASSERT_STR_EQ(t.line_text(1), "");
}

TEST(screen_erase_line_to_end) {
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHIJ");
    t.feed("\033[5G");  // col 5 (1-based)
    t.feed("\033[K");   // erase to end of line
    ASSERT_STR_EQ(t.line_text(0), "ABCD");
}

TEST(screen_erase_line_to_start) {
    TestTerminal t(10, 5);
    t.feed("ABCDEFGHIJ");
    t.feed("\033[5G");  // col 5
    t.feed("\033[1K");  // erase to start
    // Cols 0-4 erased, cols 5-9 remain
    const Line &l = t.screen.line(0);
    ASSERT_EQ(l.cells[0].codepoint, (uint32_t)' ');
    ASSERT_EQ(l.cells[4].codepoint, (uint32_t)' ');
    ASSERT_EQ(l.cells[5].codepoint, (uint32_t)'F');
}

// --- Scrolling ---

TEST(screen_scroll_on_newline) {
    TestTerminal t(10, 3);
    t.feed("Line1\r\nLine2\r\nLine3\r\nLine4");
    // Line1 should have scrolled into scrollback
    ASSERT_STR_EQ(t.line_text(0), "Line2");
    ASSERT_STR_EQ(t.line_text(1), "Line3");
    ASSERT_STR_EQ(t.line_text(2), "Line4");
    ASSERT_EQ(t.screen.scrollback_count(), 1);
}

TEST(screen_scroll_region) {
    TestTerminal t(10, 5);
    t.feed("Line1\r\nLine2\r\nLine3\r\nLine4\r\nLine5");
    t.feed("\033[2;4r");   // scroll region rows 2-4
    t.feed("\033[4;1H");   // cursor to row 4 (bottom of region)
    t.feed("\r\nNew");     // should scroll within region
    ASSERT_STR_EQ(t.line_text(0), "Line1");  // unchanged (outside region)
    ASSERT_STR_EQ(t.line_text(4), "Line5");  // unchanged (outside region)
}

TEST(screen_scroll_up_explicit) {
    TestTerminal t(10, 3);
    t.feed("AAA\r\nBBB\r\nCCC");
    t.feed("\033[1S");  // scroll up 1
    ASSERT_STR_EQ(t.line_text(0), "BBB");
    ASSERT_STR_EQ(t.line_text(1), "CCC");
    ASSERT_STR_EQ(t.line_text(2), "");
}

TEST(screen_scroll_down_explicit) {
    TestTerminal t(10, 3);
    t.feed("AAA\r\nBBB\r\nCCC");
    t.feed("\033[1T");  // scroll down 1
    ASSERT_STR_EQ(t.line_text(0), "");
    ASSERT_STR_EQ(t.line_text(1), "AAA");
    ASSERT_STR_EQ(t.line_text(2), "BBB");
}

// --- Insert / Delete ---

TEST(screen_insert_chars) {
    TestTerminal t(10, 3);
    t.feed("ABCDEF");
    t.feed("\033[3G");    // col 3 (1-based) = col 2
    t.feed("\033[2@");    // insert 2 chars
    ASSERT_EQ(t.screen.cell(0, 0).codepoint, (uint32_t)'A');
    ASSERT_EQ(t.screen.cell(0, 1).codepoint, (uint32_t)'B');
    ASSERT_EQ(t.screen.cell(0, 2).codepoint, (uint32_t)' ');
    ASSERT_EQ(t.screen.cell(0, 3).codepoint, (uint32_t)' ');
    ASSERT_EQ(t.screen.cell(0, 4).codepoint, (uint32_t)'C');
}

TEST(screen_delete_chars) {
    TestTerminal t(10, 3);
    t.feed("ABCDEF");
    t.feed("\033[3G");   // col 3
    t.feed("\033[2P");   // delete 2 chars
    ASSERT_EQ(t.screen.cell(0, 0).codepoint, (uint32_t)'A');
    ASSERT_EQ(t.screen.cell(0, 1).codepoint, (uint32_t)'B');
    ASSERT_EQ(t.screen.cell(0, 2).codepoint, (uint32_t)'E');
    ASSERT_EQ(t.screen.cell(0, 3).codepoint, (uint32_t)'F');
}

TEST(screen_insert_lines) {
    TestTerminal t(10, 5);
    t.feed("Line1\r\nLine2\r\nLine3");
    t.feed("\033[2;1H");  // cursor to row 2
    t.feed("\033[1L");    // insert 1 line
    ASSERT_STR_EQ(t.line_text(0), "Line1");
    ASSERT_STR_EQ(t.line_text(1), "");
    ASSERT_STR_EQ(t.line_text(2), "Line2");
    ASSERT_STR_EQ(t.line_text(3), "Line3");
}

TEST(screen_delete_lines) {
    TestTerminal t(10, 5);
    t.feed("Line1\r\nLine2\r\nLine3");
    t.feed("\033[2;1H");  // cursor to row 2
    t.feed("\033[1M");    // delete 1 line
    ASSERT_STR_EQ(t.line_text(0), "Line1");
    ASSERT_STR_EQ(t.line_text(1), "Line3");
}

// --- SGR attributes ---

TEST(screen_sgr_bold) {
    TestTerminal t;
    t.feed("\033[1mA\033[0mB");
    ASSERT_TRUE(t.screen.cell(0, 0).attrs & ATTR_BOLD);
    ASSERT_FALSE(t.screen.cell(0, 1).attrs & ATTR_BOLD);
}

TEST(screen_sgr_fg_256) {
    TestTerminal t;
    t.feed("\033[38;5;196mA");
    uint32_t fg = t.screen.cell(0, 0).fg;
    ASSERT_FALSE(fg & COLOR_FLAG_DEFAULT);
    ASSERT_FALSE(fg & COLOR_FLAG_TRUECOLOR);
    ASSERT_EQ(fg & 0xFF, 196u);
}

TEST(screen_sgr_fg_truecolor) {
    TestTerminal t;
    t.feed("\033[38;2;255;128;0mA");
    uint32_t fg = t.screen.cell(0, 0).fg;
    ASSERT_TRUE(fg & COLOR_FLAG_TRUECOLOR);
    ASSERT_EQ((fg >> 16) & 0xFF, 255u);
    ASSERT_EQ((fg >> 8) & 0xFF, 128u);
    ASSERT_EQ(fg & 0xFF, 0u);
}

TEST(screen_sgr_reset) {
    TestTerminal t;
    t.feed("\033[1;3;4;31mA\033[mB");
    ASSERT_TRUE(t.screen.cell(0, 0).attrs & ATTR_BOLD);
    ASSERT_TRUE(t.screen.cell(0, 0).attrs & ATTR_ITALIC);
    ASSERT_FALSE(t.screen.cell(0, 1).attrs & ATTR_BOLD);
    ASSERT_FALSE(t.screen.cell(0, 1).attrs & ATTR_ITALIC);
    ASSERT_TRUE(t.screen.cell(0, 1).fg & COLOR_FLAG_DEFAULT);
}

// --- Modes ---

TEST(screen_alt_screen) {
    TestTerminal t(10, 3);
    t.feed("Normal");
    t.feed("\033[?1049h");  // switch to alt
    ASSERT_STR_EQ(t.line_text(0), "");  // alt screen is clear
    t.feed("Alt");
    ASSERT_STR_EQ(t.line_text(0), "Alt");
    t.feed("\033[?1049l");  // switch back
    ASSERT_STR_EQ(t.line_text(0), "Normal");  // original content restored
}

TEST(screen_bracketed_paste_mode) {
    TestTerminal t;
    ASSERT_FALSE(t.screen.bracketed_paste());
    t.feed("\033[?2004h");
    ASSERT_TRUE(t.screen.bracketed_paste());
    t.feed("\033[?2004l");
    ASSERT_FALSE(t.screen.bracketed_paste());
}

TEST(screen_cursor_visibility) {
    TestTerminal t;
    ASSERT_TRUE(t.screen.cursor_visible());
    t.feed("\033[?25l");
    ASSERT_FALSE(t.screen.cursor_visible());
    t.feed("\033[?25h");
    ASSERT_TRUE(t.screen.cursor_visible());
}

TEST(screen_app_cursor_keys) {
    TestTerminal t;
    ASSERT_FALSE(t.screen.app_cursor_keys());
    t.feed("\033[?1h");
    ASSERT_TRUE(t.screen.app_cursor_keys());
}

// --- ESC sequences ---

TEST(screen_save_restore_cursor) {
    TestTerminal t;
    t.feed("\033[5;10H");  // row 5, col 10
    t.feed("\0337");       // save
    t.feed("\033[1;1H");   // move to origin
    ASSERT_EQ(t.screen.cursor_row(), 0);
    t.feed("\0338");       // restore
    ASSERT_EQ(t.screen.cursor_row(), 4);
    ASSERT_EQ(t.screen.cursor_col(), 9);
}

TEST(screen_reverse_index) {
    TestTerminal t(10, 3);
    t.feed("Line1\r\nLine2\r\nLine3");
    t.feed("\033[1;1H");  // go to top
    t.feed("\033M");      // reverse index — should scroll down
    ASSERT_STR_EQ(t.line_text(0), "");
    ASSERT_STR_EQ(t.line_text(1), "Line1");
    ASSERT_STR_EQ(t.line_text(2), "Line2");
}

// --- OSC ---

TEST(screen_osc_title) {
    TestTerminal t;
    t.feed("\033]0;my terminal\007");
    ASSERT_STR_EQ(t.last_title, "my terminal");
}

// --- Scrollback & viewport ---

TEST(screen_scrollback_viewport) {
    TestTerminal t(10, 3);
    // Fill 5 lines into a 3-row terminal → 2 go to scrollback
    t.feed("L1\r\nL2\r\nL3\r\nL4\r\nL5");
    ASSERT_EQ(t.screen.scrollback_count(), 2);
    ASSERT_TRUE(t.screen.at_bottom());

    // Scroll up into history
    t.screen.scroll_viewport(-2);
    ASSERT_FALSE(t.screen.at_bottom());
    ASSERT_STR_EQ(t.line_text(0), "L1");
    ASSERT_STR_EQ(t.line_text(1), "L2");
    ASSERT_STR_EQ(t.line_text(2), "L3");

    // Scroll back down
    t.screen.scroll_to_bottom();
    ASSERT_TRUE(t.screen.at_bottom());
    ASSERT_STR_EQ(t.line_text(0), "L3");
}

// --- Resize ---

TEST(screen_resize_grow) {
    TestTerminal t(10, 3);
    t.feed("Hello");
    t.screen.resize(20, 5);
    ASSERT_EQ(t.screen.cols(), 20);
    ASSERT_EQ(t.screen.rows(), 5);
    ASSERT_STR_EQ(t.line_text(0), "Hello");
}

TEST(screen_resize_shrink_rows) {
    TestTerminal t(10, 5);
    t.feed("L1\r\nL2\r\nL3\r\nL4\r\nL5");
    ASSERT_EQ(t.screen.cursor_row(), 4);
    t.screen.resize(10, 3);
    // Cursor was at row 4, which exceeds 3 rows, so lines pushed to scrollback
    ASSERT_EQ(t.screen.rows(), 3);
    ASSERT_EQ(t.screen.scrollback_count(), 2);
}

// --- Auto-wrap ---

TEST(screen_autowrap) {
    TestTerminal t(5, 3);
    t.feed("ABCDEFGH");
    ASSERT_STR_EQ(t.line_text(0), "ABCDE");
    ASSERT_STR_EQ(t.line_text(1), "FGH");
    ASSERT_EQ(t.screen.cursor_row(), 1);
    ASSERT_EQ(t.screen.cursor_col(), 3);
}

// --- Kitty keyboard protocol ---

TEST(kitty_kbd_push_pop) {
    TestTerminal t;
    ASSERT_FALSE(t.screen.kitty_kbd_active());
    ASSERT_EQ(t.screen.kitty_kbd_flags(), 0);

    t.feed("\033[>1u");  // push flags=1 (disambiguate)
    ASSERT_TRUE(t.screen.kitty_kbd_active());
    ASSERT_EQ(t.screen.kitty_kbd_flags(), 1);

    t.feed("\033[>3u");  // push flags=3 (disambiguate + report events)
    ASSERT_EQ(t.screen.kitty_kbd_flags(), 3);

    t.feed("\033[<1u");  // pop 1
    ASSERT_EQ(t.screen.kitty_kbd_flags(), 1);

    t.feed("\033[<1u");  // pop 1
    ASSERT_EQ(t.screen.kitty_kbd_flags(), 0);
    ASSERT_FALSE(t.screen.kitty_kbd_active());
}

TEST(kitty_kbd_pop_empty) {
    TestTerminal t;
    t.feed("\033[<5u");  // pop from empty stack — should not crash
    ASSERT_EQ(t.screen.kitty_kbd_flags(), 0);
}

TEST(kitty_kbd_query) {
    TestTerminal t;
    std::string response;
    t.screen.on_write_back = [&](const std::string &s) { response = s; };

    t.feed("\033[?u");  // query
    ASSERT_STR_EQ(response, "\033[?0u");

    t.feed("\033[>5u");  // push flags=5
    response.clear();
    t.feed("\033[?u");
    ASSERT_STR_EQ(response, "\033[?5u");
}

TEST(kitty_kbd_reset_on_alt_screen) {
    TestTerminal t;
    t.feed("\033[>1u");  // push kitty mode
    ASSERT_TRUE(t.screen.kitty_kbd_active());
    // Note: kitty spec says the stack persists across alt screen switches
    // but apps should clean up. We just verify it doesn't crash.
    t.feed("\033[?1049h");
    t.feed("\033[?1049l");
}

int main() {
    return run_tests();
}
