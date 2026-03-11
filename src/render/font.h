#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <fontconfig/fontconfig.h>
#include <string>
#include <vector>
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
    std::pair<int, uint32_t> find_glyph(uint32_t codepoint) const;

private:
    bool load_face(const std::string &path, int face_index = 0);
    std::string find_font(const std::string &family, int weight, int slant);
    void compute_metrics();

    FT_Library m_ft_lib = nullptr;

    struct FaceEntry {
        FT_Face ft_face = nullptr;
        hb_font_t *hb_font = nullptr;
        std::string path;
    };
    std::vector<FaceEntry> m_faces;

    FontMetrics m_metrics{};
    float m_size_pt = 12.0f;
    float m_dpi = 96.0f;
};

} // namespace rivt
