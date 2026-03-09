#include "render/font.h"
#include <cmath>
#include <stdexcept>

namespace rivt {

Font::Font() {
    if (FT_Init_FreeType(&ft_lib_))
        throw std::runtime_error("Failed to init FreeType");
}

Font::~Font() {
    for (auto &entry : faces_) {
        if (entry.hb_font) hb_font_destroy(entry.hb_font);
        if (entry.ft_face) FT_Done_Face(entry.ft_face);
    }
    if (ft_lib_) FT_Done_FreeType(ft_lib_);
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
    if (FT_New_Face(ft_lib_, path.c_str(), face_index, &entry.ft_face))
        return false;

    int size_26_6 = (int)(size_pt_ * 64.0f);
    FT_Set_Char_Size(entry.ft_face, size_26_6, 0, (int)dpi_, 0);

    entry.hb_font = hb_ft_font_create_referenced(entry.ft_face);
    faces_.push_back(entry);
    return true;
}

bool Font::init(const std::string &family, float size_pt, float dpi) {
    size_pt_ = size_pt;
    dpi_ = dpi;

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
    size_pt_ = size_pt;
    dpi_ = dpi;
    int size_26_6 = (int)(size_pt_ * 64.0f);
    for (auto &entry : faces_) {
        FT_Set_Char_Size(entry.ft_face, size_26_6, 0, (int)dpi_, 0);
        if (entry.hb_font) {
            hb_font_destroy(entry.hb_font);
            entry.hb_font = hb_ft_font_create_referenced(entry.ft_face);
        }
    }
    compute_metrics();
}

void Font::compute_metrics() {
    if (faces_.empty()) return;
    FT_Face f = faces_[0].ft_face;

    // Cell width from '0' or 'M'
    FT_Load_Char(f, '0', FT_LOAD_NO_BITMAP);
    metrics_.cell_width = (int)std::ceil(f->glyph->advance.x / 64.0);

    metrics_.ascender = (int)std::ceil(f->size->metrics.ascender / 64.0);
    metrics_.descender = (int)std::ceil(std::abs(f->size->metrics.descender / 64.0));
    int height = (int)std::ceil(f->size->metrics.height / 64.0);
    metrics_.line_gap = height - metrics_.ascender - metrics_.descender;
    metrics_.cell_height = metrics_.ascender + metrics_.descender + metrics_.line_gap;

    metrics_.underline_position = (int)std::ceil(f->underline_position / 64.0);
    metrics_.underline_thickness = std::max(1, (int)std::ceil(f->underline_thickness / 64.0));
}

FT_Face Font::face(int index) const {
    if (index >= 0 && index < (int)faces_.size())
        return faces_[index].ft_face;
    return nullptr;
}

hb_font_t *Font::hb_font(int index) const {
    if (index >= 0 && index < (int)faces_.size())
        return faces_[index].hb_font;
    return nullptr;
}

std::pair<int, uint32_t> Font::find_glyph(uint32_t codepoint) const {
    for (int i = 0; i < (int)faces_.size(); i++) {
        uint32_t glyph_id = FT_Get_Char_Index(faces_[i].ft_face, codepoint);
        if (glyph_id != 0)
            return {i, glyph_id};
    }
    // Return .notdef from primary font
    return {0, 0};
}

} // namespace rivt
