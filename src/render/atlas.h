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
    unsigned int texture_id() const { return m_texture_id; }
    int texture_size() const { return m_tex_size; }

    // Invalidate all (e.g. on font size change)
    void clear();

private:
    bool insert_glyph(Font &font, int font_index, uint32_t glyph_id, GlyphEntry &entry);
    void grow_texture();
    void upload_region(int x, int y, int w, int h, const uint8_t *data, GlyphType type);

    unsigned int m_texture_id = 0;
    int m_tex_size = 0;

    // Shelf packing state
    int m_shelf_x = 0;
    int m_shelf_y = 0;
    int m_shelf_height = 0;

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

    std::unordered_map<GlyphKey, GlyphEntry, KeyHash> m_cache;
};

} // namespace rivt
