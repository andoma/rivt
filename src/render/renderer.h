#pragma once
#include "render/font.h"
#include "render/atlas.h"
#include "core/types.h"
#include "core/config.h"
#include <vector>
#include <string>

namespace rivt {

class ScreenBuffer;
class TabManager;

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(const Config &config);
    void set_viewport(int width, int height);
    void set_focused(bool focused) { focused_ = focused; }

    // Original single-buffer render (still works for simple case)
    void render(const ScreenBuffer &buffer, const Config &config);

    // Render a single pane's screen buffer at an offset, clipped to the given rect
    void render_pane(const ScreenBuffer &buffer, const Config &config,
                     int offset_x, int offset_y, int w, int h,
                     bool pane_focused);

    // Render border between panes
    void render_border(float x, float y, float w, float h,
                       float r, float g, float b);

    // Render dot grid in dead zone (tmux constrained area)
    void render_dot_grid(int offset_x, int offset_y, int w, int h,
                         int cell_w, int cell_h);

    // Render tab bar
    void render_tab_bar(const TabManager &tabs, const Config &config, int bar_height);

    // Hit-test tab bar, returns tab index or -1
    int tab_hit_test(const TabManager &tabs, int x);

    // Flush all accumulated vertices (call after building all quads)
    void flush();

    // Clear screen
    void clear(const Config &config);

    // Begin a new frame (clear vertices, set up projection)
    void begin_frame(const Config &config);

    void draw_text(float x, float y, const std::string &text,
                   float r, float g, float b, float atlas_size);

    Font &font() { return font_; }
    void set_font_size(float size_pt, float dpi);
    const FontMetrics &metrics() const { return font_.metrics(); }

    // Compute grid dimensions from pixel size
    void compute_grid(int pixel_w, int pixel_h, int &cols, int &rows) const;

    int viewport_w() const { return viewport_w_; }
    int viewport_h() const { return viewport_h_; }

private:
    void build_shaders();

    void resolve_color(uint32_t encoded, const Config &config,
                       float &r, float &g, float &b) const;

    // Build vertices for a screen buffer with pixel offset
    void build_pane_vertices(const ScreenBuffer &buffer, const Config &config,
                             int offset_x, int offset_y, int clip_w, int clip_h,
                             bool pane_focused);

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
