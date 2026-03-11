#include "render/font.h"
#include <cmath>
#include <stdexcept>

namespace rivt {

Font::Font() {
    if (FT_Init_FreeType(&m_ft_lib))
        throw std::runtime_error("Failed to init FreeType");
}

Font::~Font() {
    for (auto &entry : m_faces) {
        if (entry.hb_font) hb_font_destroy(entry.hb_font);
        if (entry.ft_face) FT_Done_Face(entry.ft_face);
    }
    if (m_ft_lib) FT_Done_FreeType(m_ft_lib);
}

std::string Font::find_font(const std::string &family, int weight, int slant) {
    FcConfig *config = FcInitLoadConfigAndFonts();
    FcPattern *pattern = FcPatternCreate();

    if (!family.empty())
        FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)family.c_str());
    else
        FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)"monospace");

    FcPatternAddInteger(pattern, FC_WEIGHT, weight);
    FcPatternAddInteger(pattern, FC_SLANT, slant);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);

    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result;
    FcPattern *match = FcFontMatch(config, pattern, &result);

    std::string path;
    if (match) {
        FcChar8 *file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            path = (const char *)file;
        FcPatternDestroy(match);
    }

    FcPatternDestroy(pattern);
    FcConfigDestroy(config);
    return path;
}

bool Font::load_face(const std::string &path, int face_index) {
    FaceEntry entry;
    entry.path = path;
    if (FT_New_Face(m_ft_lib, path.c_str(), face_index, &entry.ft_face))
        return false;

    int size_26_6 = (int)(m_size_pt * 64.0f);
    FT_Set_Char_Size(entry.ft_face, size_26_6, 0, (int)m_dpi, 0);

    entry.hb_font = hb_ft_font_create_referenced(entry.ft_face);
    m_faces.push_back(entry);
    return true;
}

bool Font::init(const std::string &family, float size_pt, float dpi) {
    m_size_pt = size_pt;
    m_dpi = dpi;

    // Primary monospace font
    std::string primary = find_font(family, FC_WEIGHT_REGULAR, FC_SLANT_ROMAN);
    if (primary.empty() || !load_face(primary))
        return false;

    // Fallback: generic sans for symbols
    std::string sans_fallback = find_font("sans-serif", FC_WEIGHT_REGULAR, FC_SLANT_ROMAN);
    if (!sans_fallback.empty() && sans_fallback != primary)
        load_face(sans_fallback);

    // Fallback: emoji
    std::string emoji = find_font("emoji", FC_WEIGHT_REGULAR, FC_SLANT_ROMAN);
    if (!emoji.empty() && emoji != primary && emoji != sans_fallback)
        load_face(emoji);

    compute_metrics();
    return true;
}

void Font::set_size(float size_pt, float dpi) {
    m_size_pt = size_pt;
    m_dpi = dpi;
    int size_26_6 = (int)(m_size_pt * 64.0f);
    for (auto &entry : m_faces) {
        FT_Set_Char_Size(entry.ft_face, size_26_6, 0, (int)m_dpi, 0);
        if (entry.hb_font) {
            hb_font_destroy(entry.hb_font);
            entry.hb_font = hb_ft_font_create_referenced(entry.ft_face);
        }
    }
    compute_metrics();
}

void Font::compute_metrics() {
    if (m_faces.empty()) return;
    FT_Face f = m_faces[0].ft_face;

    // Cell width from '0' or 'M'
    FT_Load_Char(f, '0', FT_LOAD_NO_BITMAP);
    m_metrics.cell_width = (int)std::ceil(f->glyph->advance.x / 64.0);

    m_metrics.ascender = (int)std::ceil(f->size->metrics.ascender / 64.0);
    m_metrics.descender = (int)std::ceil(std::abs(f->size->metrics.descender / 64.0));
    int height = (int)std::ceil(f->size->metrics.height / 64.0);
    m_metrics.line_gap = height - m_metrics.ascender - m_metrics.descender;
    m_metrics.cell_height = m_metrics.ascender + m_metrics.descender + m_metrics.line_gap;

    m_metrics.underline_position = (int)std::ceil(f->underline_position / 64.0);
    m_metrics.underline_thickness = std::max(1, (int)std::ceil(f->underline_thickness / 64.0));
}

FT_Face Font::face(int index) const {
    if (index >= 0 && index < (int)m_faces.size())
        return m_faces[index].ft_face;
    return nullptr;
}

hb_font_t *Font::hb_font(int index) const {
    if (index >= 0 && index < (int)m_faces.size())
        return m_faces[index].hb_font;
    return nullptr;
}

std::pair<int, uint32_t> Font::find_glyph(uint32_t codepoint) const {
    for (int i = 0; i < (int)m_faces.size(); i++) {
        uint32_t glyph_id = FT_Get_Char_Index(m_faces[i].ft_face, codepoint);
        if (glyph_id != 0)
            return {i, glyph_id};
    }
    // Return .notdef from primary font
    return {0, 0};
}

} // namespace rivt
