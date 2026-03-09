#include "test.h"
#include "terminal/vt_parser.h"
#include <vector>

using namespace rivt;

// Test handler that records all callbacks
struct RecordingHandler : public VtHandler {
    struct PrintEvent { uint32_t cp; };
    struct ExecuteEvent { uint8_t code; };
    struct CsiEvent { CsiParams params; char intermediate; char final_byte; };
    struct OscEvent { int command; std::string payload; };
    struct EscEvent { char intermediate; char final_byte; };

    std::vector<PrintEvent> prints;
    std::vector<ExecuteEvent> executes;
    std::vector<CsiEvent> csis;
    std::vector<OscEvent> oscs;
    std::vector<EscEvent> escs;
    int dcs_starts = 0;
    int dcs_ends = 0;
    std::string dcs_data;

    void print(uint32_t cp) override { prints.push_back({cp}); }
    void execute(uint8_t code) override { executes.push_back({code}); }
    void csi_dispatch(const CsiParams &params, char inter, char fb) override {
        csis.push_back({params, inter, fb});
    }
    void osc_dispatch(int cmd, const std::string &payload) override {
        oscs.push_back({cmd, payload});
    }
    void esc_dispatch(char inter, char fb) override {
        escs.push_back({inter, fb});
    }
    void dcs_start() override { dcs_starts++; }
    void dcs_put(uint8_t byte) override { dcs_data += (char)byte; }
    void dcs_end() override { dcs_ends++; }

    void clear() {
        prints.clear(); executes.clear(); csis.clear();
        oscs.clear(); escs.clear();
        dcs_starts = 0; dcs_ends = 0; dcs_data.clear();
    }
};

static void feed(VtParser &p, const char *s) {
    p.feed(s, strlen(s));
}

// --- Print tests ---

TEST(print_ascii) {
    RecordingHandler h; VtParser p(h);
    feed(p, "Hello");
    ASSERT_EQ(h.prints.size(), 5u);
    ASSERT_EQ(h.prints[0].cp, (uint32_t)'H');
    ASSERT_EQ(h.prints[4].cp, (uint32_t)'o');
}

TEST(print_utf8_2byte) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\xc3\xa9");  // é (U+00E9)
    ASSERT_EQ(h.prints.size(), 1u);
    ASSERT_EQ(h.prints[0].cp, 0x00E9u);
}

TEST(print_utf8_3byte) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\xe2\x9c\x93");  // ✓ (U+2713)
    ASSERT_EQ(h.prints.size(), 1u);
    ASSERT_EQ(h.prints[0].cp, 0x2713u);
}

TEST(print_utf8_4byte) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\xf0\x9f\x98\x80");  // 😀 (U+1F600)
    ASSERT_EQ(h.prints.size(), 1u);
    ASSERT_EQ(h.prints[0].cp, 0x1F600u);
}

TEST(print_mixed_ascii_utf8) {
    RecordingHandler h; VtParser p(h);
    feed(p, "a\xc3\xa9" "b");  // a é b
    ASSERT_EQ(h.prints.size(), 3u);
    ASSERT_EQ(h.prints[0].cp, (uint32_t)'a');
    ASSERT_EQ(h.prints[1].cp, 0x00E9u);
    ASSERT_EQ(h.prints[2].cp, (uint32_t)'b');
}

// --- C0 control tests ---

TEST(execute_cr_lf) {
    RecordingHandler h; VtParser p(h);
    feed(p, "A\r\nB");
    ASSERT_EQ(h.prints.size(), 2u);
    ASSERT_EQ(h.executes.size(), 2u);
    ASSERT_EQ(h.executes[0].code, 0x0D);  // CR
    ASSERT_EQ(h.executes[1].code, 0x0A);  // LF
}

TEST(execute_tab) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\t");
    ASSERT_EQ(h.executes.size(), 1u);
    ASSERT_EQ(h.executes[0].code, 0x09);
}

TEST(execute_backspace) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\x08");
    ASSERT_EQ(h.executes.size(), 1u);
    ASSERT_EQ(h.executes[0].code, 0x08);
}

TEST(execute_bell) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\x07");
    ASSERT_EQ(h.executes.size(), 1u);
    ASSERT_EQ(h.executes[0].code, 0x07);
}

// --- CSI tests ---

TEST(csi_cursor_up) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[5A");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].final_byte, 'A');
    ASSERT_EQ(h.csis[0].params.get(0, 1), 5);
}

TEST(csi_cursor_position) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[10;20H");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].final_byte, 'H');
    ASSERT_EQ(h.csis[0].params.get(0, 1), 10);
    ASSERT_EQ(h.csis[0].params.get(1, 1), 20);
}

TEST(csi_no_params_defaults) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[H");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].params.count(), 0);
    ASSERT_EQ(h.csis[0].params.get(0, 1), 1);  // default
    ASSERT_EQ(h.csis[0].params.get(1, 1), 1);
}

TEST(csi_sgr_basic) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[1;31m");  // bold + red fg
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].final_byte, 'm');
    ASSERT_EQ(h.csis[0].params.get(0), 1);
    ASSERT_EQ(h.csis[0].params.get(1), 31);
}

TEST(csi_sgr_256color) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[38;5;196m");  // fg 256-color index 196
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].params.get(0), 38);
    ASSERT_EQ(h.csis[0].params.get(1), 5);
    ASSERT_EQ(h.csis[0].params.get(2), 196);
}

TEST(csi_sgr_truecolor) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[38;2;255;128;0m");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].params.get(0), 38);
    ASSERT_EQ(h.csis[0].params.get(1), 2);
    ASSERT_EQ(h.csis[0].params.get(2), 255);
    ASSERT_EQ(h.csis[0].params.get(3), 128);
    ASSERT_EQ(h.csis[0].params.get(4), 0);
}

TEST(csi_sub_params_colon) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[4:3m");  // curly underline
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].params.params[0].value, 4);
    ASSERT_EQ(h.csis[0].params.params[0].sub.size(), 1u);
    ASSERT_EQ(h.csis[0].params.params[0].sub[0], 3);
}

TEST(csi_private_mode) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[?1049h");  // DECSET alt screen
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].intermediate, '?');
    ASSERT_EQ(h.csis[0].final_byte, 'h');
    ASSERT_EQ(h.csis[0].params.get(0), 1049);
}

TEST(csi_erase_display) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[2J");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].final_byte, 'J');
    ASSERT_EQ(h.csis[0].params.get(0), 2);
}

TEST(csi_scroll_region) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[5;20r");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].final_byte, 'r');
    ASSERT_EQ(h.csis[0].params.get(0), 5);
    ASSERT_EQ(h.csis[0].params.get(1), 20);
}

TEST(csi_multiple_sequences) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[1A\033[2B\033[3C");
    ASSERT_EQ(h.csis.size(), 3u);
    ASSERT_EQ(h.csis[0].final_byte, 'A');
    ASSERT_EQ(h.csis[1].final_byte, 'B');
    ASSERT_EQ(h.csis[2].final_byte, 'C');
    ASSERT_EQ(h.csis[0].params.get(0, 1), 1);
    ASSERT_EQ(h.csis[1].params.get(0, 1), 2);
    ASSERT_EQ(h.csis[2].params.get(0, 1), 3);
}

// --- OSC tests ---

TEST(osc_set_title_bel) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033]0;My Title\007");
    ASSERT_EQ(h.oscs.size(), 1u);
    ASSERT_EQ(h.oscs[0].command, 0);
    ASSERT_STR_EQ(h.oscs[0].payload, "My Title");
}

TEST(osc_set_title_st) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033]2;Another Title\033\\");
    ASSERT_EQ(h.oscs.size(), 1u);
    ASSERT_EQ(h.oscs[0].command, 2);
    ASSERT_STR_EQ(h.oscs[0].payload, "Another Title");
}

TEST(osc_working_directory) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033]7;file:///home/user\033\\");
    ASSERT_EQ(h.oscs.size(), 1u);
    ASSERT_EQ(h.oscs[0].command, 7);
    ASSERT_STR_EQ(h.oscs[0].payload, "file:///home/user");
}

// --- ESC tests ---

TEST(esc_save_cursor) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\0337");  // DECSC
    ASSERT_EQ(h.escs.size(), 1u);
    ASSERT_EQ(h.escs[0].intermediate, 0);
    ASSERT_EQ(h.escs[0].final_byte, '7');
}

TEST(esc_restore_cursor) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\0338");  // DECRC
    ASSERT_EQ(h.escs.size(), 1u);
    ASSERT_EQ(h.escs[0].final_byte, '8');
}

TEST(esc_index) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033D");  // IND
    ASSERT_EQ(h.escs.size(), 1u);
    ASSERT_EQ(h.escs[0].final_byte, 'D');
}

TEST(esc_reverse_index) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033M");  // RI
    ASSERT_EQ(h.escs.size(), 1u);
    ASSERT_EQ(h.escs[0].final_byte, 'M');
}

// --- DCS tests ---

TEST(dcs_passthrough) {
    RecordingHandler h; VtParser p(h);
    feed(p, "\033Phello\033\\");
    ASSERT_EQ(h.dcs_starts, 1);
    ASSERT_EQ(h.dcs_ends, 1);
    // 'h' transitions from DcsEntry, then 'ello' is passthrough data
    ASSERT_TRUE(h.dcs_data.find("ello") != std::string::npos);
}

// --- Mixed content tests ---

TEST(mixed_text_and_csi) {
    RecordingHandler h; VtParser p(h);
    feed(p, "AB\033[1;31mCD\033[0mEF");
    ASSERT_EQ(h.prints.size(), 6u);  // A B C D E F
    ASSERT_EQ(h.csis.size(), 2u);    // SGR bold+red, SGR reset
    ASSERT_EQ(h.prints[0].cp, (uint32_t)'A');
    ASSERT_EQ(h.prints[5].cp, (uint32_t)'F');
}

TEST(incremental_feed) {
    // Feed a CSI sequence byte by byte
    RecordingHandler h; VtParser p(h);
    feed(p, "\033");
    ASSERT_EQ(h.csis.size(), 0u);
    feed(p, "[");
    ASSERT_EQ(h.csis.size(), 0u);
    feed(p, "5");
    ASSERT_EQ(h.csis.size(), 0u);
    feed(p, "A");
    ASSERT_EQ(h.csis.size(), 1u);
    ASSERT_EQ(h.csis[0].params.get(0, 1), 5);
}

TEST(c0_interrupts_csi) {
    // C0 controls (except ESC) should be executed during CSI parsing
    RecordingHandler h; VtParser p(h);
    feed(p, "\033[\nA");
    ASSERT_EQ(h.executes.size(), 1u);
    ASSERT_EQ(h.executes[0].code, 0x0A);
}

int main() {
    return run_tests();
}
