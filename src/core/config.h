#pragma once
#include <cstdint>
#include <string>

namespace rivt {

struct Config {
    // Scrollback
    int scrollback_lines = 10000;

    // Font
    std::string font_family;  // empty = system default monospace
    float font_size = 10.0f;

    // Colors - default dark theme
    uint32_t fg_color = 0xD4D4D4;
    uint32_t bg_color = 0x1E1E1E;
    uint32_t cursor_color = 0xAEAFAD;
    uint32_t selection_color = 0x264F78;

    // Standard 16 colors (dark theme)
    uint32_t palette[256];

    // Behavior
    bool visual_bell = true;
    bool auto_scroll_on_output = true;  // snap to bottom only if already at bottom
    bool bracketed_paste = true;
    bool focus_reporting = true;
    bool url_detection = true;
    bool osc52_write = true;
    bool osc52_read = false;

    // Window
    int initial_cols = 80;
    int initial_rows = 24;

    Config();
};

} // namespace rivt
