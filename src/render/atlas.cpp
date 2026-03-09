#include "render/atlas.h"
#include <GL/gl.h>
#include <cstring>
#include <vector>
#include <algorithm>

namespace rivt {

GlyphAtlas::GlyphAtlas() = default;

GlyphAtlas::~GlyphAtlas() {
    if (texture_id_)
        glDeleteTextures(1, &texture_id_);
}

bool GlyphAtlas::init(int initial_size) {
    tex_size_ = initial_size;

    glGenTextures(1, &texture_id_);
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_size_, tex_size_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    shelf_x_ = 0;
    shelf_y_ = 0;
    shelf_height_ = 0;

    return true;
}

void GlyphAtlas::clear() {
    cache_.clear();
    shelf_x_ = 0;
    shelf_y_ = 0;
    shelf_height_ = 0;

    // Clear texture
    glBindTexture(GL_TEXTURE_2D, texture_id_);
    std::vector<uint8_t> zeros(tex_size_ * tex_size_ * 4, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_size_, tex_size_,
                    GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());
}

const GlyphEntry *GlyphAtlas::get(Font &font, int font_index, uint32_t glyph_id) {
    GlyphKey key{font_index, glyph_id};
    auto it = cache_.find(key);
    if (it != cache_.end())
        return &it->second;

    GlyphEntry entry{};
    if (!insert_glyph(font, font_index, glyph_id, entry))
        return nullptr;

    auto [ins, _] = cache_.emplace(key, entry);
    return &ins->second;
}

void GlyphAtlas::upload_region(int x, int y, int w, int h, const uint8_t *data, GlyphType type) {
    glBindTexture(GL_TEXTURE_2D, texture_id_);

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

bool GlyphAtlas::insert_glyph(Font &font, int font_index, uint32_t glyph_id, GlyphEntry &entry) {
    FT_Face face = font.face(font_index);
    if (!face) return false;

    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    bool is_color = FT_HAS_COLOR(face);
    if (is_color)
        load_flags |= FT_LOAD_COLOR;

    if (FT_Load_Glyph(face, glyph_id, load_flags))
        return false;

    FT_Render_Mode render_mode = FT_RENDER_MODE_NORMAL;
    if (is_color)
        render_mode = FT_RENDER_MODE_NORMAL;

    if (FT_Render_Glyph(face->glyph, render_mode))
        return false;

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

    // Calculate atlas width for this glyph
    int atlas_w = bw;
    if (type == GlyphType::Lcd)
        atlas_w = bw / 3;

    // Shelf packing
    if (shelf_x_ + atlas_w + 1 > tex_size_) {
        // Move to next shelf
        shelf_x_ = 0;
        shelf_y_ += shelf_height_ + 1;
        shelf_height_ = 0;
    }

    if (shelf_y_ + bh > tex_size_) {
        grow_texture();
    }

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

    upload_region(shelf_x_, shelf_y_, bw, bh, buffer.data(), type);

    entry.u = shelf_x_;
    entry.v = shelf_y_;
    entry.w = atlas_w;
    entry.h = bh;
    entry.bearing_x = face->glyph->bitmap_left;
    entry.bearing_y = face->glyph->bitmap_top;
    entry.type = type;

    shelf_x_ += atlas_w + 1;
    shelf_height_ = std::max(shelf_height_, bh);

    return true;
}

void GlyphAtlas::grow_texture() {
    // Double the texture size and re-upload
    // For simplicity, just clear and let everything re-rasterize
    int new_size = tex_size_ * 2;

    glBindTexture(GL_TEXTURE_2D, texture_id_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, new_size, new_size, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    tex_size_ = new_size;
    cache_.clear();
    shelf_x_ = 0;
    shelf_y_ = 0;
    shelf_height_ = 0;
}

} // namespace rivt
