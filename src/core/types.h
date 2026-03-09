#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace rivt {

// Color encoding: bit 24 set = truecolor RGB in low 24 bits
//                 bit 24 clear = palette index in low 8 bits
// Bit 25 = default color flag
constexpr uint32_t COLOR_FLAG_TRUECOLOR = 1u << 24;
constexpr uint32_t COLOR_FLAG_DEFAULT   = 1u << 25;

inline uint32_t color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return COLOR_FLAG_TRUECOLOR | (r << 16) | (g << 8) | b;
}

inline uint32_t color_palette(uint8_t idx) {
    return idx;
}

inline uint32_t color_default() {
    return COLOR_FLAG_DEFAULT;
}

// Cell attribute bits
enum CellAttr : uint16_t {
    ATTR_BOLD             = 1 << 0,
    ATTR_DIM              = 1 << 1,
    ATTR_ITALIC           = 1 << 2,
    ATTR_UNDERLINE        = 1 << 3,  // basic underline
    ATTR_UNDERLINE_DOUBLE = 1 << 4,
    ATTR_UNDERLINE_CURLY  = 1 << 5,
    ATTR_STRIKETHROUGH    = 1 << 6,
    ATTR_INVERSE          = 1 << 7,
    ATTR_HIDDEN           = 1 << 8,
    ATTR_WIDE             = 1 << 9,
    ATTR_WIDE_CONT        = 1 << 10,
    ATTR_WRAP             = 1 << 11,
    ATTR_HYPERLINK        = 1 << 12,
};

constexpr uint16_t ATTR_UNDERLINE_MASK = ATTR_UNDERLINE | ATTR_UNDERLINE_DOUBLE | ATTR_UNDERLINE_CURLY;

constexpr uint32_t GRAPHEME_SENTINEL_BASE = 0x110000;

struct Cell {
    uint32_t codepoint = ' ';
    uint32_t fg = COLOR_FLAG_DEFAULT;
    uint32_t bg = COLOR_FLAG_DEFAULT;
    uint16_t attrs = 0;

    void reset() {
        codepoint = ' ';
        fg = COLOR_FLAG_DEFAULT;
        bg = COLOR_FLAG_DEFAULT;
        attrs = 0;
    }
};

struct Line {
    std::vector<Cell> cells;
    bool wrapped = false;
    bool dirty = true;
    uint32_t semantic_zone = 0;

    explicit Line(int cols = 80) : cells(cols) {}

    void resize(int cols) {
        cells.resize(cols);
        dirty = true;
    }
};

// Platform event types
enum class KeyMod : uint8_t {
    NoMod   = 0,
    Shift   = 1 << 0,
    Ctrl    = 1 << 1,
    Alt     = 1 << 2,
    Super   = 1 << 3,
};

inline KeyMod operator|(KeyMod a, KeyMod b) {
    return static_cast<KeyMod>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool operator&(KeyMod a, KeyMod b) {
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

struct KeyEvent {
    uint32_t keysym;
    KeyMod mods;
    std::string text;  // UTF-8 text input (empty for non-printable)
    bool pressed;
};

enum class MouseButton : uint8_t {
    NoButton = 0, Left = 1, Middle = 2, Right = 3,
    ScrollUp = 4, ScrollDown = 5,
};

struct MouseEvent {
    int x, y;          // pixel coordinates
    MouseButton button;
    KeyMod mods;
    bool pressed;
    bool motion;
};

} // namespace rivt
