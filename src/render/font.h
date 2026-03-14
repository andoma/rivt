#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <fontconfig/fontconfig.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

namespace rivt {

struct FontMetrics {
    int cell_width;
    int cell_height;
    int ascender;
    int descender;
    int line_gap;
    int underline_position;
    int underline_thickness;
};

class Font {
public:
    Font();
    ~Font();

    bool init(const std::string &family, float size_pt, float dpi = 96.0f);
    void set_size(float size_pt, float dpi = 96.0f);

    FT_Face face(int index = 0) const;
    hb_font_t *hb_font(int index = 0) const;
    int font_count() const { return (int)m_faces.size(); }

    const FontMetrics &metrics() const { return m_metrics; }

    // Find glyph in fallback chain, returns (font_index, glyph_id)
    // May load new fallback fonts on demand via fontconfig
    std::pair<int, uint32_t> find_glyph(uint32_t codepoint);

private:
    bool load_face(const std::string &path, int face_index = 0);
    std::string find_font(const std::string &family, int weight, int slant);
    std::string find_font_for_codepoint(uint32_t codepoint);
    void compute_metrics();

    FT_Library m_ft_lib = nullptr;

    struct FaceEntry {
        FT_Face ft_face = nullptr;
        hb_font_t *hb_font = nullptr;
        std::string path;
        bool fixed_strike = false;  // true for bitmap-only fonts (e.g. color emoji)
    };
    std::vector<FaceEntry> m_faces;

    FontMetrics m_metrics{};
    float m_size_pt = 12.0f;
    float m_dpi = 96.0f;

    // Tracks font paths already loaded or rejected to avoid repeated lookups
    std::unordered_set<std::string> m_loaded_paths;

    // Codepoints for which fontconfig lookup already failed — avoids expensive repeated queries
    std::unordered_set<uint32_t> m_failed_codepoints;
};

} // namespace rivt
