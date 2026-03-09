#pragma once
#include "render/font.h"
#include <unordered_map>
#include <cstdint>

namespace rivt {

enum class GlyphType : uint8_t {
    Mask,   // grayscale alpha
    Lcd,    // subpixel (3-channel alpha)
    Color,  // BGRA emoji
};

struct GlyphEntry {
    int u, v;           // position in atlas texture
    int w, h;           // dimensions in atlas
    int bearing_x;
    int bearing_y;
    GlyphType type;
};

class GlyphAtlas {
public:
    GlyphAtlas();
    ~GlyphAtlas();

    bool init(int initial_size = 1024);

    // Get or rasterize glyph, returns entry
    const GlyphEntry *get(Font &font, int font_index, uint32_t glyph_id);

    // GL texture
    unsigned int texture_id() const { return texture_id_; }
    int texture_size() const { return tex_size_; }

    // Invalidate all (e.g. on font size change)
    void clear();

private:
    bool insert_glyph(Font &font, int font_index, uint32_t glyph_id, GlyphEntry &entry);
    void grow_texture();
    void upload_region(int x, int y, int w, int h, const uint8_t *data, GlyphType type);

    unsigned int texture_id_ = 0;
    int tex_size_ = 0;

    // Shelf packing state
    int shelf_x_ = 0;
    int shelf_y_ = 0;
    int shelf_height_ = 0;

    struct GlyphKey {
        int font_index;
        uint32_t glyph_id;
        bool operator==(const GlyphKey &o) const {
            return font_index == o.font_index && glyph_id == o.glyph_id;
        }
    };

    struct KeyHash {
        size_t operator()(const GlyphKey &k) const {
            return std::hash<uint64_t>()(((uint64_t)k.font_index << 32) | k.glyph_id);
        }
    };

    std::unordered_map<GlyphKey, GlyphEntry, KeyHash> cache_;
};

} // namespace rivt
