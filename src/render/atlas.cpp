#include "render/atlas.h"
#include "core/debug.h"
#include <ft2build.h>
#include FT_SYNTHESIS_H
#include "render/gl.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

#ifdef __APPLE__
#include <CoreText/CoreText.h>
#include <cstdlib>
#endif

namespace rivt {

GlyphAtlas::GlyphAtlas() = default;

GlyphAtlas::~GlyphAtlas() {
#ifdef __APPLE__
    release_ct_fonts();
#endif
    if (m_texture_id)
        glDeleteTextures(1, &m_texture_id);
}

bool GlyphAtlas::init(int initial_size) {
#ifdef __APPLE__
    release_ct_fonts();
#endif
    m_tex_size = initial_size;

    glGenTextures(1, &m_texture_id);
    glBindTexture(GL_TEXTURE_2D, m_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_tex_size, m_tex_size, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_shelf_x = 0;
    m_shelf_y = 0;
    m_shelf_height = 0;

    return true;
}

void GlyphAtlas::clear() {
#ifdef __APPLE__
    // Font size changed: CT fonts are sized per ppem, so recreate them.
    release_ct_fonts();
#endif
    m_cache.clear();
    m_shelf_x = 0;
    m_shelf_y = 0;
    m_shelf_height = 0;

    // Clear texture
    glBindTexture(GL_TEXTURE_2D, m_texture_id);
    std::vector<uint8_t> zeros(m_tex_size * m_tex_size * 4, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_tex_size, m_tex_size,
                    GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
}

const GlyphEntry *GlyphAtlas::get(Font &font, int font_index, uint32_t glyph_id, bool bold) {
    GlyphKey key{font_index, glyph_id, bold};
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return &it->second;

    GlyphEntry entry{};
    if (!insert_glyph(font, font_index, glyph_id, bold, entry))
        return nullptr;

    auto [ins, _] = m_cache.emplace(key, entry);
    return &ins->second;
}

void GlyphAtlas::upload_region(int x, int y, int w, int h, const uint8_t *data, GlyphType type) {
    glBindTexture(GL_TEXTURE_2D, m_texture_id);

    if (type == GlyphType::Color) {
        // BGRA data → convert to RGBA
        std::vector<uint8_t> rgba(w * h * 4);
        for (int i = 0; i < w * h; i++) {
            rgba[i * 4 + 0] = data[i * 4 + 2]; // R ← B
            rgba[i * 4 + 1] = data[i * 4 + 1]; // G
            rgba[i * 4 + 2] = data[i * 4 + 0]; // B ← R
            rgba[i * 4 + 3] = data[i * 4 + 3]; // A
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else if (type == GlyphType::Mask) {
        // Single-channel alpha → expand to RGBA (0,0,0,alpha)
        std::vector<uint8_t> rgba(w * h * 4);
        for (int i = 0; i < w * h; i++) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = data[i];
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else {
        // LCD subpixel: 3x wide grayscale → store as RGB in atlas
        // data is w*3 bytes per row (each row has 3 sub-pixels per pixel)
        int lcd_w = w / 3;  // actual pixel width
        std::vector<uint8_t> rgba(lcd_w * h * 4);
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < lcd_w; col++) {
                int src = row * w + col * 3;
                int dst = (row * lcd_w + col) * 4;
                rgba[dst + 0] = data[src + 0]; // R sub-pixel alpha
                rgba[dst + 1] = data[src + 1]; // G sub-pixel alpha
                rgba[dst + 2] = data[src + 2]; // B sub-pixel alpha
                rgba[dst + 3] = 255;
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, lcd_w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
}

bool GlyphAtlas::insert_glyph(Font &font, int font_index, uint32_t glyph_id, bool bold, GlyphEntry &entry) {
    FT_Face face = font.face(font_index);
    if (!face) {
        dbg("atlas: insert_glyph font_idx=%d glyph=%u — no face", font_index, glyph_id);
        return false;
    }

    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    bool is_color = FT_HAS_COLOR(face);
    if (is_color)
        load_flags |= FT_LOAD_COLOR;

#ifdef __APPLE__
    if (!is_color && insert_glyph_ct(font, font_index, glyph_id, bold, entry))
        return true;
#endif

    FT_Error err = FT_Load_Glyph(face, glyph_id, load_flags);
    if (err) {
        dbg("atlas: FT_Load_Glyph failed font_idx=%d glyph=%u err=%d is_color=%d",
            font_index, glyph_id, err, is_color);
        return false;
    }

    if (bold && !is_color)
        FT_GlyphSlot_Embolden(face->glyph);

    FT_Render_Mode render_mode = FT_RENDER_MODE_NORMAL;
    if (is_color)
        render_mode = FT_RENDER_MODE_NORMAL;

    err = FT_Render_Glyph(face->glyph, render_mode);
    if (err) {
        dbg("atlas: FT_Render_Glyph failed font_idx=%d glyph=%u err=%d pixel_mode=%d",
            font_index, glyph_id, err, face->glyph->bitmap.pixel_mode);
        return false;
    }

    FT_Bitmap &bmp = face->glyph->bitmap;
    int bw = bmp.width;
    int bh = bmp.rows;

    if (bw == 0 || bh == 0) {
        // Empty glyph (space, etc)
        entry = {0, 0, 0, 0,
                 face->glyph->bitmap_left, face->glyph->bitmap_top,
                 GlyphType::Mask};
        return true;
    }

    // Determine type
    GlyphType type;
    if (bmp.pixel_mode == FT_PIXEL_MODE_BGRA)
        type = GlyphType::Color;
    else if (bmp.pixel_mode == FT_PIXEL_MODE_LCD)
        type = GlyphType::Lcd;
    else
        type = GlyphType::Mask;

    // Ensure contiguous bitmap data
    std::vector<uint8_t> buffer;
    int bytes_per_pixel = (type == GlyphType::Color) ? 4 : 1;
    int row_bytes = bw * bytes_per_pixel;
    buffer.resize(row_bytes * bh);
    for (int row = 0; row < bh; row++) {
        memcpy(buffer.data() + row * row_bytes,
               bmp.buffer + row * bmp.pitch,
               row_bytes);
    }

    place(buffer.data(), bw, bh, face->glyph->bitmap_left, face->glyph->bitmap_top,
          type, entry);
    return true;
}

void GlyphAtlas::place(const uint8_t *data, int bw, int bh, int bearing_x, int bearing_y,
                       GlyphType type, GlyphEntry &entry) {
    // Calculate atlas width for this glyph
    int atlas_w = bw;
    if (type == GlyphType::Lcd)
        atlas_w = bw / 3;

    // Shelf packing
    if (m_shelf_x + atlas_w + 1 > m_tex_size) {
        // Move to next shelf
        m_shelf_x = 0;
        m_shelf_y += m_shelf_height + 1;
        m_shelf_height = 0;
    }

    if (m_shelf_y + bh > m_tex_size) {
        grow_texture();
    }

    upload_region(m_shelf_x, m_shelf_y, bw, bh, data, type);

    entry.u = m_shelf_x;
    entry.v = m_shelf_y;
    entry.w = atlas_w;
    entry.h = bh;
    entry.bearing_x = bearing_x;
    entry.bearing_y = bearing_y;
    entry.type = type;

    m_shelf_x += atlas_w + 1;
    m_shelf_height = std::max(m_shelf_height, bh);
}

#ifdef __APPLE__

namespace {

// RIVT_RASTER=freetype disables Core Text rasterization.
bool core_text_enabled() {
    static bool on = [] {
        const char *e = getenv("RIVT_RASTER");
        return !(e && !strcmp(e, "freetype"));
    }();
    return on;
}

// Core Text font smoothing makes strokes heavier. Like iTerm2's default
// ("thin strokes on Retina"), use it only on non-Retina displays.
// RIVT_FONT_SMOOTH=0/1 forces it off or on.
bool font_smoothing(float dpi) {
    if (const char *e = getenv("RIVT_FONT_SMOOTH"))
        return atoi(e) != 0;
    return dpi < 144.0f;
}

CTFontRef create_ct_font(const std::string &path, FT_Face face) {
    if (path.empty() || !face->size) return nullptr;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, (const UInt8 *)path.data(), (CFIndex)path.size(), false);
    if (!url) return nullptr;
    CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url);
    CFRelease(url);
    if (!descs) return nullptr;

    CFIndex n = CFArrayGetCount(descs);
    if (n == 0) {
        CFRelease(descs);
        return nullptr;
    }

    // For collections (.ttc), match the FreeType face by PostScript name
    // so glyph IDs refer to the same face.
    CFIndex pick = std::min<CFIndex>(face->face_index & 0xffff, n - 1);
    if (const char *ps = FT_Get_Postscript_Name(face)) {
        CFStringRef want = CFStringCreateWithCString(nullptr, ps, kCFStringEncodingUTF8);
        for (CFIndex i = 0; want && i < n; i++) {
            auto d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, i);
            auto name = (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute);
            bool match = name && CFStringCompare(name, want, 0) == kCFCompareEqualTo;
            if (name) CFRelease(name);
            if (match) {
                pick = i;
                break;
            }
        }
        if (want) CFRelease(want);
    }

    // Same ppem as FreeType, so glyphs line up with the FreeType metrics.
    double ppem = face->units_per_EM * (double)face->size->metrics.x_scale / 65536.0 / 64.0;
    auto desc = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, pick);
    CTFontRef ct = CTFontCreateWithFontDescriptor(desc, ppem, nullptr);
    CFRelease(descs);
    return ct;
}

} // namespace

void GlyphAtlas::release_ct_fonts() {
    for (auto &[idx, f] : m_ct_fonts)
        if (f) CFRelease((CTFontRef)f);
    m_ct_fonts.clear();
}

const void *GlyphAtlas::ct_font(Font &font, int font_index) {
    auto it = m_ct_fonts.find(font_index);
    if (it != m_ct_fonts.end())
        return it->second;
    CTFontRef ct = create_ct_font(font.path(font_index), font.face(font_index));
    if (!ct)
        dbg("atlas: no Core Text font for font_idx=%d, using FreeType", font_index);
    m_ct_fonts[font_index] = ct;
    return ct;
}

bool GlyphAtlas::insert_glyph_ct(Font &font, int font_index, uint32_t glyph_id, bool bold,
                                 GlyphEntry &entry) {
    if (!core_text_enabled()) return false;
    auto ct = (CTFontRef)ct_font(font, font_index);
    if (!ct) return false;

    CGGlyph g = (CGGlyph)glyph_id;
    CGRect r = CTFontGetBoundingRectsForGlyphs(ct, kCTFontOrientationHorizontal, &g, nullptr, 1);
    if (CGRectIsEmpty(r)) {
        entry = {0, 0, 0, 0, 0, 0, GlyphType::Mask};
        return true;
    }

    // Synthetic bold: same strength as FT_GlyphSlot_Embolden (ppem / 24).
    CGFloat stroke = bold ? CTFontGetSize(ct) / 24.0 : 0.0;

    // Pad for the bold stroke and for the dilation from font smoothing.
    CGFloat pad = stroke / 2 + 1;
    int left   = (int)std::floor(CGRectGetMinX(r) - pad);
    int bottom = (int)std::floor(CGRectGetMinY(r) - pad);
    int right  = (int)std::ceil(CGRectGetMaxX(r) + pad);
    int top    = (int)std::ceil(CGRectGetMaxY(r) + pad);
    int w = right - left;
    int h = top - bottom;

    // White glyph on black: the gray value is the coverage.
    std::vector<uint8_t> buf(w * h, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(buf.data(), w, h, 8, w, cs, kCGImageAlphaNone);
    CGColorSpaceRelease(cs);
    if (!ctx) return false;

    CGContextSetAllowsAntialiasing(ctx, true);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetAllowsFontSmoothing(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, font_smoothing(font.dpi()));
    CGContextSetAllowsFontSubpixelPositioning(ctx, false);
    CGContextSetShouldSubpixelPositionFonts(ctx, false);
    CGContextSetGrayFillColor(ctx, 1, 1);
    if (bold) {
        CGContextSetGrayStrokeColor(ctx, 1, 1);
        CGContextSetLineWidth(ctx, stroke);
        CGContextSetTextDrawingMode(ctx, kCGTextFillStroke);
    }

    CGPoint pos = CGPointMake(-left, -bottom);
    CTFontDrawGlyphs(ct, &g, &pos, 1, ctx);
    CGContextRelease(ctx);

    // Bitmap rows run top to bottom, as FreeType's do.
    place(buf.data(), w, h, left, top, GlyphType::Mask, entry);
    return true;
}

#endif // __APPLE__

void GlyphAtlas::grow_texture() {
    // Double the texture size and re-upload
    // For simplicity, just clear and let everything re-rasterize
    int new_size = m_tex_size * 2;

    glBindTexture(GL_TEXTURE_2D, m_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, new_size, new_size, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    m_tex_size = new_size;
    m_cache.clear();
    m_shelf_x = 0;
    m_shelf_y = 0;
    m_shelf_height = 0;
}

} // namespace rivt
