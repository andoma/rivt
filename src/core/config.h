#pragma once
#include <cstdint>
#include <string>

namespace rivt {

// macOS renders at a larger effective point size, so 9 reads right there;
// on Linux 11 matches the pre-ecb213a look.
#ifdef __APPLE__
inline constexpr float kDefaultFontSize = 9.0f;
#else
inline constexpr float kDefaultFontSize = 11.0f;
#endif

struct Config {
    // Scrollback
    int scrollback_lines = 10000;

    // Font
    std::string font_family;  // empty = system default monospace
    float font_size = kDefaultFontSize;

    // Colors - default dark theme
    uint32_t fg_color = 0xD4D4D4;
    uint32_t bg_color = 0x0F0F0F;
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
