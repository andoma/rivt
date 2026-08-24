#include "test.h"
#include "terminal/vt_parser.h"
#include "terminal/screen_buffer.h"
#include "proto/snapshot.h"
#include "proto/frame.h"

using namespace rivt;

struct TestTerminal {
    ScreenBuffer screen;
    VtParser parser;

    TestTerminal(int cols = 80, int rows = 24, int scrollback = 1000)
        : screen(cols, rows, scrollback), parser(screen) {}

    void feed(const std::string &s) { parser.feed(s.data(), s.size()); }
};

static bool cells_equal(const Cell &a, const Cell &b) {
    return a.codepoint == b.codepoint && a.fg == b.fg && a.bg == b.bg &&
           a.attrs == b.attrs && a.hyperlink_id == b.hyperlink_id;
}

// Full observable-state comparison between two terminals.
static bool terminals_equal(const ScreenBuffer &a, const ScreenBuffer &b, const char *ctx) {
    #define CHECK(cond, what) \
        if (!(cond)) { fprintf(stderr, "  [%s] mismatch: %s\n", ctx, what); return false; }
    CHECK(a.cols() == b.cols() && a.rows() == b.rows(), "geometry");
    CHECK(a.cursor_row() == b.cursor_row() && a.cursor_col() == b.cursor_col(), "cursor");
    CHECK(a.cursor_visible() == b.cursor_visible(), "cursor visibility");
    CHECK(a.alt_screen() == b.alt_screen(), "alt screen flag");
    CHECK(a.bracketed_paste() == b.bracketed_paste(), "bracketed paste");
    CHECK(a.app_cursor_keys() == b.app_cursor_keys(), "app cursor");
    CHECK(a.focus_reporting() == b.focus_reporting(), "focus reporting");
    CHECK(a.mouse_mode() == b.mouse_mode(), "mouse mode");
    CHECK(a.sgr_mouse() == b.sgr_mouse(), "sgr mouse");
    CHECK(a.kitty_kbd_flags() == b.kitty_kbd_flags(), "kitty kbd flags");
    CHECK(a.scrollback_count() == b.scrollback_count(), "scrollback count");
    for (int r = 0; r < a.rows(); r++) {
        const Line &la = a.line(r), &lb = b.line(r);
        CHECK(la.wrapped == lb.wrapped, "wrapped flag");
        CHECK(la.cells.size() == lb.cells.size(), "line width");
        for (size_t c = 0; c < la.cells.size(); c++)
            if (!cells_equal(la.cells[c], lb.cells[c])) {
                fprintf(stderr, "  [%s] cell (%d,%zu): cp %x/%x fg %x/%x attrs %x/%x\n",
                        ctx, r, c, la.cells[c].codepoint, lb.cells[c].codepoint,
                        la.cells[c].fg, lb.cells[c].fg, la.cells[c].attrs, lb.cells[c].attrs);
                return false;
            }
    }
    for (int i = 0; i < a.scrollback_count(); i++) {
        const Line &la = a.scrollback_line(i), &lb = b.scrollback_line(i);
        CHECK(la.cells.size() == lb.cells.size(), "scrollback line width");
        for (size_t c = 0; c < la.cells.size(); c++)
            CHECK(cells_equal(la.cells[c], lb.cells[c]), "scrollback cell");
    }
    #undef CHECK
    return true;
}

// The core property: feeding (prefix + suffix) continuously must equal
// feeding prefix, snapshotting, restoring into a fresh terminal, and
// feeding suffix there. Proves grid, modes, charsets, and parser
// transient state all survive a snapshot at an arbitrary byte boundary.
static bool split_equivalent(const std::string &prefix, const std::string &suffix,
                             const char *ctx, int cols = 80, int rows = 24) {
    TestTerminal cont(cols, rows);
    cont.feed(prefix);
    cont.feed(suffix);

    TestTerminal src(cols, rows);
    src.feed(prefix);
    auto blob = proto::Snapshot::serialize(src.screen, src.parser);

    TestTerminal dst(8, 3);  // wrong initial size on purpose
    if (!proto::Snapshot::deserialize(dst.screen, dst.parser, blob.data(), blob.size())) {
        fprintf(stderr, "  [%s] deserialize failed\n", ctx);
        return false;
    }
    dst.feed(suffix);
    return terminals_equal(cont.screen, dst.screen, ctx);
}

TEST(split_stream_equivalence) {
    struct Case { const char *name, *prefix, *suffix; };
    static const Case cases[] = {
        {"plain", "Hello ", "World"},
        {"sgr", "\033[31;1mRed ", "\033[42mGreenBg\033[0m done"},
        {"csi_split", "pre\033[", "31mX"},
        {"csi_split_param", "\033[38;2;10;", "20;30mtruecolor"},
        {"osc_split", "\033]0;ti", "tle\007text"},
        {"utf8_split", "caf\xC3", "\xA9 latte"},
        {"wide_utf8_split", "\xE6\x97", "\xA5\xE6\x9C\xAC"},
        {"dec_graphics", "\033(0", "qqq\033(Bplain"},
        {"alt_screen", "\033[?1049habc", "def\033[?1049lback"},
        {"newlines", "line1\r\nline2\r\n", "line3"},
        {"kitty_push", "\033[>1u", "after"},
        {"scroll_region", "\033[2;4r\033[2;1H", "a\r\nb\r\nc\r\nd"},
        {"save_restore_cursor", "\0337\033[5;10H", "\0338X"},
        {"rep_split", "A\033[", "3bZ"},
        {"modes", "\033[?2004h\033[?1h\033[?1002h\033[?1006h", "x"},
        {"esc_split", "abc\033", "Mup"},
        {"insert_delete", "abcdef\033[3D\033[2@", "XY\033[P"},
    };
    for (const auto &c : cases)
        ASSERT_TRUE(split_equivalent(c.prefix, c.suffix, c.name));
    ASSERT_TRUE(split_equivalent(std::string(85, 'w'), "tail", "wrap"));
}

TEST(scrollback_roundtrip) {
    TestTerminal t(20, 5);
    for (int i = 0; i < 30; i++)
        t.feed("line" + std::to_string(i) + "\r\n");
    ASSERT_TRUE(t.screen.scrollback_count() > 20);

    auto blob = proto::Snapshot::serialize(t.screen, t.parser);
    TestTerminal r(20, 5);
    ASSERT_TRUE(proto::Snapshot::deserialize(r.screen, r.parser, blob.data(), blob.size()));
    ASSERT_TRUE(terminals_equal(t.screen, r.screen, "scrollback_full"));
}

TEST(scrollback_tail_limit) {
    TestTerminal t(20, 5);
    for (int i = 0; i < 30; i++)
        t.feed("line" + std::to_string(i) + "\r\n");
    int total = t.screen.scrollback_count();

    auto blob = proto::Snapshot::serialize(t.screen, t.parser, 10);
    TestTerminal r(20, 5);
    ASSERT_TRUE(proto::Snapshot::deserialize(r.screen, r.parser, blob.data(), blob.size()));
    ASSERT_EQ(r.screen.scrollback_count(), 10);
    ASSERT_TRUE(total > 10);
    // Most-recent 10 lines match (index 0 = most recent).
    for (int i = 0; i < 10; i++) {
        const Line &la = t.screen.scrollback_line(i), &lb = r.screen.scrollback_line(i);
        for (size_t c = 0; c < la.cells.size(); c++)
            ASSERT_TRUE(cells_equal(la.cells[c], lb.cells[c]));
    }
}

TEST(hyperlink_roundtrip) {
    TestTerminal t;
    t.feed("\033]8;;https://example.com\007link\033]8;;\007 plain");
    auto blob = proto::Snapshot::serialize(t.screen, t.parser);
    TestTerminal r;
    ASSERT_TRUE(proto::Snapshot::deserialize(r.screen, r.parser, blob.data(), blob.size()));
    uint16_t id = r.screen.line(0).cells[0].hyperlink_id;
    ASSERT_TRUE(id != 0);
    ASSERT_STR_EQ(r.screen.hyperlink_uri(id), "https://example.com");
    // New hyperlinks after restore get fresh ids that keep working.
    r.feed("\033]8;;https://other.org\007x");
    uint16_t id2 = r.screen.line(0).cells[r.screen.cursor_col() - 1].hyperlink_id;
    ASSERT_TRUE(id2 != 0);
    ASSERT_STR_EQ(r.screen.hyperlink_uri(id2), "https://other.org");
}

TEST(restore_marks_dirty) {
    TestTerminal t;
    t.feed("content");
    auto blob = proto::Snapshot::serialize(t.screen, t.parser);
    TestTerminal r;
    r.feed("old");
    ASSERT_TRUE(proto::Snapshot::deserialize(r.screen, r.parser, blob.data(), blob.size()));
    ASSERT_TRUE(r.screen.any_dirty());
    ASSERT_EQ(r.screen.viewport_offset(), 0);
}

TEST(reject_garbage) {
    TestTerminal r;
    std::vector<uint8_t> junk = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_FALSE(proto::Snapshot::deserialize(r.screen, r.parser, junk.data(), junk.size()));
    TestTerminal t;
    t.feed("x");
    auto blob = proto::Snapshot::serialize(t.screen, t.parser);
    // Truncations must fail cleanly, never crash (ASan build catches overreads).
    for (size_t cut = 0; cut < blob.size(); cut += 7)
        proto::Snapshot::deserialize(r.screen, r.parser, blob.data(), cut);
}

TEST(prepend_scrollback_stable_coordinates) {
    TestTerminal t(20, 5);
    for (int i = 0; i < 30; i++)
        t.feed("line" + std::to_string(i) + "\r\n");

    // Simulate a remote replica: restore from a tail-limited snapshot.
    auto blob = proto::Snapshot::serialize(t.screen, t.parser, 10);
    TestTerminal r(20, 5);
    ASSERT_TRUE(proto::Snapshot::deserialize(r.screen, r.parser, blob.data(), blob.size()));
    int trimmed = r.screen.scrollback_trimmed();
    ASSERT_TRUE(trimmed > 0);
    ASSERT_EQ(r.screen.scrollback_count(), 10);

    // Select something and remember its text (absolute coordinates).
    r.screen.selection.active = true;
    r.screen.selection.start_line = trimmed + 2;
    r.screen.selection.end_line = trimmed + 2;
    r.screen.selection.start_col = 0;
    r.screen.selection.end_col = 19;
    std::string before = r.screen.get_selection_text();
    ASSERT_TRUE(!before.empty());

    // Prepend 5 older lines, as a scrollback-fetch chunk would.
    std::vector<Line> older;
    for (int i = 0; i < 5; i++) {
        Line l(20);
        l.cells[0].codepoint = 'O';
        older.push_back(std::move(l));
    }
    r.screen.prepend_scrollback(std::move(older));

    ASSERT_EQ(r.screen.scrollback_count(), 15);
    ASSERT_EQ(r.screen.scrollback_trimmed(), trimmed - 5);
    // Absolute coordinates unaffected: same selection, same text.
    ASSERT_STR_EQ(r.screen.get_selection_text(), before);
    // The prepended lines are the oldest held lines now.
    ASSERT_EQ(r.screen.scrollback_line(14).cells[0].codepoint, (uint32_t)'O');
}

TEST(frame_header_roundtrip) {
    proto::FrameHeader h{12345, 7, 3}, out{};
    uint8_t buf[proto::FRAME_HEADER_SIZE];
    proto::encode_frame_header(buf, h);
    ASSERT_TRUE(proto::decode_frame_header(buf, out));
    ASSERT_EQ(out.len, 12345u);
    ASSERT_EQ(out.channel, 7);
    ASSERT_EQ(out.type, 3);
    h.len = proto::FRAME_MAX_LEN + 1;
    proto::encode_frame_header(buf, h);
    ASSERT_FALSE(proto::decode_frame_header(buf, out));
}

int main() { return run_tests(); }
