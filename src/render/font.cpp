#include "render/font.h"
#include "core/debug.h"
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
    if (m_loaded_paths.count(path))
        return false;
    m_loaded_paths.insert(path);

    FaceEntry entry;
    entry.path = path;
    if (FT_New_Face(m_ft_lib, path.c_str(), face_index, &entry.ft_face))
        return false;

    int size_26_6 = (int)(m_size_pt * 64.0f);
    if (FT_Set_Char_Size(entry.ft_face, size_26_6, 0, (int)m_dpi, 0)) {
        // Scalable sizing failed — try selecting a fixed bitmap strike
        // (common for color emoji fonts like Noto Color Emoji)
        if (entry.ft_face->num_fixed_sizes > 0) {
            int best = 0;
            int target_height = (int)(m_size_pt * m_dpi / 72.0f);
            int best_diff = abs(entry.ft_face->available_sizes[0].height - target_height);
            for (int i = 1; i < entry.ft_face->num_fixed_sizes; i++) {
                int diff = abs(entry.ft_face->available_sizes[i].height - target_height);
                if (diff < best_diff) {
                    best_diff = diff;
                    best = i;
                }
            }
            FT_Select_Size(entry.ft_face, best);
            entry.fixed_strike = true;
        }
    }

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
        if (!entry.fixed_strike)
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

std::string Font::find_font_for_codepoint(uint32_t codepoint) {
    FcConfig *config = FcInitLoadConfigAndFonts();
    FcPattern *pattern = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    FcCharSetAddChar(cs, codepoint);
    FcPatternAddCharSet(pattern, FC_CHARSET, cs);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);

    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result;
    FcPattern *match = FcFontMatch(config, pattern, &result);

    std::string path;
    if (match) {
        // Verify the matched font actually contains the codepoint
        FcCharSet *match_cs = nullptr;
        if (FcPatternGetCharSet(match, FC_CHARSET, 0, &match_cs) == FcResultMatch
            && FcCharSetHasChar(match_cs, codepoint)) {
            FcChar8 *file = nullptr;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
                path = (const char *)file;
        }
        FcPatternDestroy(match);
    }

    FcCharSetDestroy(cs);
    FcPatternDestroy(pattern);
    FcConfigDestroy(config);
    return path;
}

std::pair<int, uint32_t> Font::find_glyph(uint32_t codepoint) {
    for (int i = 0; i < (int)m_faces.size(); i++) {
        uint32_t glyph_id = FT_Get_Char_Index(m_faces[i].ft_face, codepoint);
        if (glyph_id != 0)
            return {i, glyph_id};
    }

    // Don't retry codepoints that already failed fontconfig lookup
    if (m_failed_codepoints.count(codepoint))
        return {0, 0};

    // Ask fontconfig for a font that covers this codepoint
    std::string path = find_font_for_codepoint(codepoint);
    if (!path.empty() && load_face(path)) {
        int idx = (int)m_faces.size() - 1;
        uint32_t glyph_id = FT_Get_Char_Index(m_faces[idx].ft_face, codepoint);
        if (glyph_id != 0) {
            dbg("font: loaded fallback %s for U+%04X", path.c_str(), codepoint);
            return {idx, glyph_id};
        }
        dbg("font: fallback %s matched for U+%04X but glyph not found", path.c_str(), codepoint);
    } else if (path.empty()) {
        dbg("font: no font found for U+%04X", codepoint);
    } else {
        dbg("font: fallback %s already loaded, no glyph for U+%04X", path.c_str(), codepoint);
    }

    // Remember this codepoint failed so we don't retry expensive fontconfig queries
    m_failed_codepoints.insert(codepoint);

    // Return .notdef from primary font
    return {0, 0};
}

} // namespace rivt
