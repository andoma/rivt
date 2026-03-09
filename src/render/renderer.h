#pragma once
#include "render/font.h"
#include "render/atlas.h"
#include "core/types.h"
#include "core/config.h"
#include <vector>

namespace rivt {

class ScreenBuffer;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(const Config &config);
    void set_viewport(int width, int height);
    void set_focused(bool focused) { focused_ = focused; }
    void render(const ScreenBuffer &buffer, const Config &config);

    Font &font() { return font_; }
    const FontMetrics &metrics() const { return font_.metrics(); }

    // Compute grid dimensions from pixel size
    void compute_grid(int pixel_w, int pixel_h, int &cols, int &rows) const;

private:
    void build_shaders();
    void render_background(const ScreenBuffer &buffer, const Config &config);
    void render_glyphs(const ScreenBuffer &buffer, const Config &config);
    void render_cursor(const ScreenBuffer &buffer, const Config &config);

    void resolve_color(uint32_t encoded, const Config &config,
                       float &r, float &g, float &b) const;

    Font font_;
    GlyphAtlas atlas_;

    unsigned int bg_shader_ = 0;
    unsigned int glyph_shader_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;

    int viewport_w_ = 0;
    int viewport_h_ = 0;
    bool focused_ = true;

    struct QuadVertex {
        float x, y;       // position
        float u, v;       // texcoord
        float r, g, b, a; // color
        float type;        // 0=bg, 1=mask, 2=color
    };
    std::vector<QuadVertex> vertices_;
};

} // namespace rivt
