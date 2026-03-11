#include "core/config.h"
#include <cmath>
#include <algorithm>

namespace rivt {

// sRGB <-> Linear <-> CIELAB conversion for perceptual color interpolation

struct Lab { double L, a, b; };

static double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

static double linear_to_srgb(double c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

static double xyz_f(double t) {
    return t > 0.008856 ? std::cbrt(t) : (903.3 * t + 16.0) / 116.0;
}

static double xyz_f_inv(double t) {
    return t > 0.206893 ? t * t * t : (116.0 * t - 16.0) / 903.3;
}

static Lab rgb_to_lab(uint32_t rgb) {
    double r = srgb_to_linear(((rgb >> 16) & 0xFF) / 255.0);
    double g = srgb_to_linear(((rgb >> 8) & 0xFF) / 255.0);
    double b = srgb_to_linear((rgb & 0xFF) / 255.0);

    // sRGB -> XYZ (D65)
    double x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    double z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;

    // XYZ -> Lab (D65 white point)
    double fx = xyz_f(x / 0.95047);
    double fy = xyz_f(y / 1.00000);
    double fz = xyz_f(z / 1.08883);

    return { 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz) };
}

static uint32_t lab_to_rgb(Lab lab) {
    double fy = (lab.L + 16.0) / 116.0;
    double fx = lab.a / 500.0 + fy;
    double fz = fy - lab.b / 200.0;

    double x = xyz_f_inv(fx) * 0.95047;
    double y = xyz_f_inv(fy) * 1.00000;
    double z = xyz_f_inv(fz) * 1.08883;

    // XYZ -> linear sRGB
    double r =  3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    double b =  0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    int ri = std::clamp((int)(linear_to_srgb(std::max(r, 0.0)) * 255.0 + 0.5), 0, 255);
    int gi = std::clamp((int)(linear_to_srgb(std::max(g, 0.0)) * 255.0 + 0.5), 0, 255);
    int bi = std::clamp((int)(linear_to_srgb(std::max(b, 0.0)) * 255.0 + 0.5), 0, 255);

    return (ri << 16) | (gi << 8) | bi;
}

static Lab lerp_lab(double t, Lab a, Lab b) {
    return { a.L + t * (b.L - a.L),
             a.a + t * (b.a - a.a),
             a.b + t * (b.b - a.b) };
}

Config::Config() {
    // Base 16 colors (custom dark theme)
    // Normal
    palette[0]  = 0x000000; // black
    palette[1]  = 0xCD3131; // red
    palette[2]  = 0x0DBC79; // green
    palette[3]  = 0xE5E510; // yellow
    palette[4]  = 0x2472C8; // blue
    palette[5]  = 0xBC3FBC; // magenta
    palette[6]  = 0x11A8CD; // cyan
    palette[7]  = 0xE5E5E5; // white

    // Bright
    palette[8]  = 0x666666; // bright black
    palette[9]  = 0xF14C4C; // bright red
    palette[10] = 0x23D18B; // bright green
    palette[11] = 0xF5F543; // bright yellow
    palette[12] = 0x3B8EEA; // bright blue
    palette[13] = 0xD670D6; // bright magenta
    palette[14] = 0x29B8DB; // bright cyan
    palette[15] = 0xF5F5F5; // bright white

    // Generate 216-color cube from base16 via CIELAB interpolation
    // Map the 8 corners of the RGB cube to base colors:
    //   (0,0,0)->bg, (5,0,0)->red, (0,5,0)->green, (5,5,0)->yellow
    //   (0,0,5)->blue, (5,0,5)->magenta, (0,5,5)->cyan, (5,5,5)->fg
    Lab bg_lab = rgb_to_lab(bg_color);
    Lab fg_lab = rgb_to_lab(fg_color);
    Lab base8[8] = {
        bg_lab,
        rgb_to_lab(palette[1]),  // red
        rgb_to_lab(palette[2]),  // green
        rgb_to_lab(palette[3]),  // yellow
        rgb_to_lab(palette[4]),  // blue
        rgb_to_lab(palette[5]),  // magenta
        rgb_to_lab(palette[6]),  // cyan
        fg_lab,
    };

    // Trilinear interpolation through CIELAB
    for (int ri = 0; ri < 6; ri++) {
        double r = ri / 5.0;
        Lab c0 = lerp_lab(r, base8[0], base8[1]); // black..red
        Lab c1 = lerp_lab(r, base8[2], base8[3]); // green..yellow
        Lab c2 = lerp_lab(r, base8[4], base8[5]); // blue..magenta
        Lab c3 = lerp_lab(r, base8[6], base8[7]); // cyan..white
        for (int gi = 0; gi < 6; gi++) {
            double g = gi / 5.0;
            Lab c4 = lerp_lab(g, c0, c1);
            Lab c5 = lerp_lab(g, c2, c3);
            for (int bi = 0; bi < 6; bi++) {
                double b = bi / 5.0;
                Lab c6 = lerp_lab(b, c4, c5);
                palette[16 + ri * 36 + gi * 6 + bi] = lab_to_rgb(c6);
            }
        }
    }

    // Grayscale ramp: 24 steps between bg and fg in CIELAB
    for (int i = 0; i < 24; i++) {
        double t = (i + 1) / 25.0;
        palette[232 + i] = lab_to_rgb(lerp_lab(t, bg_lab, fg_lab));
    }
}

} // namespace rivt
