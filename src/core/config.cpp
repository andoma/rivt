#include "core/config.h"

namespace rivt {

Config::Config() {
    // Standard xterm-256color palette
    // First 8: normal colors
    palette[0]  = 0x000000; // black
    palette[1]  = 0xCD3131; // red
    palette[2]  = 0x0DBC79; // green
    palette[3]  = 0xE5E510; // yellow
    palette[4]  = 0x2472C8; // blue
    palette[5]  = 0xBC3FBC; // magenta
    palette[6]  = 0x11A8CD; // cyan
    palette[7]  = 0xE5E5E5; // white

    // 8-15: bright colors
    palette[8]  = 0x666666; // bright black
    palette[9]  = 0xF14C4C; // bright red
    palette[10] = 0x23D18B; // bright green
    palette[11] = 0xF5F543; // bright yellow
    palette[12] = 0x3B8EEA; // bright blue
    palette[13] = 0xD670D6; // bright magenta
    palette[14] = 0x29B8DB; // bright cyan
    palette[15] = 0xF5F5F5; // bright white

    // 16-231: 6x6x6 color cube
    for (int i = 0; i < 216; i++) {
        int r = (i / 36) % 6;
        int g = (i / 6) % 6;
        int b = i % 6;
        r = r ? (r * 40 + 55) : 0;
        g = g ? (g * 40 + 55) : 0;
        b = b ? (b * 40 + 55) : 0;
        palette[16 + i] = (r << 16) | (g << 8) | b;
    }

    // 232-255: grayscale ramp
    for (int i = 0; i < 24; i++) {
        int v = 8 + i * 10;
        palette[232 + i] = (v << 16) | (v << 8) | v;
    }
}

} // namespace rivt
