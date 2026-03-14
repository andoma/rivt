#define GL_GLEXT_PROTOTYPES
#include "render/renderer.h"
#include "terminal/screen_buffer.h"
#include "terminal/image_store.h"
#include "core/tab_manager.h"
#include "core/debug.h"
#include <GL/gl.h>
#include <cstdio>
#include <cmath>
#include <string>

namespace rivt {

static const char *glyph_vertex_src = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aType;
out vec2 vTexCoord;
out vec4 vColor;
out float vType;
uniform mat4 uProjection;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
    vType = aType;
}
)";

static const char *glyph_fragment_src = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
in float vType;
out vec4 FragColor;
uniform sampler2D uAtlas;
void main() {
    if (vType < 0.5) {
        // Background quad
        FragColor = vColor;
    } else if (vType < 1.5) {
        // Mask glyph: sample alpha, multiply by fg color
        vec4 tex = texture(uAtlas, vTexCoord);
        FragColor = vec4(vColor.rgb, vColor.a * tex.a);
    } else {
        // Color glyph (emoji): sample RGBA directly
        FragColor = texture(uAtlas, vTexCoord);
    }
}
)";

static const char *image_vertex_src = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform mat4 uProjection;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char *image_fragment_src = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D uImage;
void main() {
    FragColor = texture(uImage, vTexCoord);
}
)";

static unsigned int compile_shader(GLenum type, const char *src) {
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, 512, nullptr, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
    }
    return shader;
}

static unsigned int link_program(unsigned int vert, unsigned int frag) {
    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    int success;
    glGetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(prog, 512, nullptr, log);
        fprintf(stderr, "Shader link error: %s\n", log);
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
    return prog;
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_bg_shader) glDeleteProgram(m_bg_shader);
    if (m_glyph_shader) glDeleteProgram(m_glyph_shader);
    if (m_image_shader) glDeleteProgram(m_image_shader);
}

bool Renderer::init(const Config &config) {
    if (!m_font.init(config.font_family, config.font_size))
        return false;

    if (!m_atlas.init(1024))
        return false;

    build_shaders();

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    return true;
}

void Renderer::build_shaders() {
    // We use a unified shader for simplicity
    auto vert = compile_shader(GL_VERTEX_SHADER, glyph_vertex_src);
    auto frag = compile_shader(GL_FRAGMENT_SHADER, glyph_fragment_src);
    m_glyph_shader = link_program(vert, frag);

    auto img_vert = compile_shader(GL_VERTEX_SHADER, image_vertex_src);
    auto img_frag = compile_shader(GL_FRAGMENT_SHADER, image_fragment_src);
    m_image_shader = link_program(img_vert, img_frag);
}

void Renderer::set_font_size(float size_pt, float dpi) {
    m_font.set_size(size_pt, dpi);
    m_atlas.clear();
}

void Renderer::set_viewport(int width, int height) {
    m_viewport_w = width;
    m_viewport_h = height;
    glViewport(0, 0, width, height);
}

void Renderer::compute_grid(int pixel_w, int pixel_h, int &cols, int &rows) const {
    const auto &m = m_font.metrics();
    cols = pixel_w / m.cell_width;
    rows = pixel_h / m.cell_height;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
}

void Renderer::resolve_color(uint32_t encoded, const Config &config,
                              float &r, float &g, float &b) const {
    if (encoded & COLOR_FLAG_DEFAULT) {
        r = g = b = 0;
        return;
    }
    uint32_t rgb;
    if (encoded & COLOR_FLAG_TRUECOLOR) {
        rgb = encoded & 0xFFFFFF;
    } else {
        uint8_t idx = encoded & 0xFF;
        rgb = config.palette[idx];
    }
    r = ((rgb >> 16) & 0xFF) / 255.0f;
    g = ((rgb >> 8) & 0xFF) / 255.0f;
    b = (rgb & 0xFF) / 255.0f;
}

void Renderer::clear(const Config &/*config*/) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void Renderer::begin_frame(const Config &config) {
    // Re-apply glViewport every frame.  set_viewport() is called from
    // handle_resize() which may fire while a *different* window's GL context
    // is current, sending the glViewport to the wrong context.  Doing it
    // here, after make_current(), guarantees the correct context.
    glViewport(0, 0, m_viewport_w, m_viewport_h);
    clear(config);
    m_vertices.clear();
}

void Renderer::build_pane_vertices(const ScreenBuffer &buffer, const Config &config,
                                    int offset_x, int offset_y, int clip_w, int clip_h,
                                    bool pane_focused) {
    const auto &m = m_font.metrics();
    float def_fg_r = ((config.fg_color >> 16) & 0xFF) / 255.0f;
    float def_fg_g = ((config.fg_color >> 8) & 0xFF) / 255.0f;
    float def_fg_b = (config.fg_color & 0xFF) / 255.0f;
    float def_bg_r = ((config.bg_color >> 16) & 0xFF) / 255.0f;
    float def_bg_g = ((config.bg_color >> 8) & 0xFF) / 255.0f;
    float def_bg_b = (config.bg_color & 0xFF) / 255.0f;

    float atlas_size = (float)m_atlas.texture_size();
    float ox = (float)offset_x;
    float oy = (float)offset_y;

    bool has_selection = buffer.selection.active;
    bool has_search = buffer.search.active && !buffer.search.matches.empty();
    float sel_bg_r = 0, sel_bg_g = 0, sel_bg_b = 0;
    if (has_selection) {
        sel_bg_r = ((config.selection_color >> 16) & 0xFF) / 255.0f;
        sel_bg_g = ((config.selection_color >> 8) & 0xFF) / 255.0f;
        sel_bg_b = (config.selection_color & 0xFF) / 255.0f;
    }

    // Pre-reserve to avoid reallocation (estimate ~12 vertices per cell)
    m_vertices.reserve(m_vertices.size() + buffer.rows() * buffer.cols() * 12);

    // Pass 1: backgrounds — emitted first so glyphs that extend beyond their
    // cell (e.g. underscore descenders) are not occluded by the next row's bg.
    for (int row = 0; row < buffer.rows(); row++) {
        const Line &line = buffer.line(row);
        float y_top = oy + row * m.cell_height;

        if (y_top >= oy + clip_h) break;

        int abs_line = buffer.absolute_line(row);

        for (int col = 0; col < buffer.cols() && col < (int)line.cells.size(); col++) {
            const Cell &cell = line.cells[col];
            float x_left = ox + col * m.cell_width;
            float x_right = x_left + m.cell_width;
            float y_bottom = y_top + m.cell_height;

            if (x_left >= ox + clip_w) break;

            float cell_bg_r, cell_bg_g, cell_bg_b;

            if (cell.bg & COLOR_FLAG_DEFAULT) {
                cell_bg_r = def_bg_r; cell_bg_g = def_bg_g; cell_bg_b = def_bg_b;
            } else {
                resolve_color(cell.bg, config, cell_bg_r, cell_bg_g, cell_bg_b);
            }

            bool selected = has_selection && buffer.selection.contains(abs_line, col);
            if (selected) {
                cell_bg_r = sel_bg_r;
                cell_bg_g = sel_bg_g;
                cell_bg_b = sel_bg_b;
            }

            int mt = has_search ? buffer.search.match_type(abs_line, col) : 0;
            if (mt == 2) {
                cell_bg_r = 0.9f; cell_bg_g = 0.6f; cell_bg_b = 0.1f;
            } else if (mt == 1) {
                cell_bg_r = 0.6f; cell_bg_g = 0.5f; cell_bg_b = 0.1f;
            }

            if (cell.attrs & ATTR_INVERSE) {
                float fg_r, fg_g, fg_b;
                if (cell.fg & COLOR_FLAG_DEFAULT) {
                    fg_r = def_fg_r; fg_g = def_fg_g; fg_b = def_fg_b;
                } else {
                    uint32_t fg_enc = cell.fg;
                    if ((cell.attrs & ATTR_BOLD) && !(fg_enc & COLOR_FLAG_TRUECOLOR) && (fg_enc & 0xFF) < 8)
                        fg_enc = (fg_enc & 0xFF) + 8;
                    resolve_color(fg_enc, config, fg_r, fg_g, fg_b);
                }
                cell_bg_r = fg_r; cell_bg_g = fg_g; cell_bg_b = fg_b;
            }

            if (!(cell.bg & COLOR_FLAG_DEFAULT) || (cell.attrs & ATTR_INVERSE) || selected || mt) {
                m_vertices.push_back({x_left,  y_top,    0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                m_vertices.push_back({x_right, y_top,    0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                m_vertices.push_back({x_right, y_bottom, 0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                m_vertices.push_back({x_left,  y_top,    0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                m_vertices.push_back({x_right, y_bottom, 0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                m_vertices.push_back({x_left,  y_bottom, 0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
            }
        }
    }

    // Pass 2: glyphs, underlines, strikethrough — drawn on top of all backgrounds.
    for (int row = 0; row < buffer.rows(); row++) {
        const Line &line = buffer.line(row);
        float y_top = oy + row * m.cell_height;

        if (y_top >= oy + clip_h) break;

        int abs_line = buffer.absolute_line(row);

        for (int col = 0; col < buffer.cols() && col < (int)line.cells.size(); col++) {
            const Cell &cell = line.cells[col];
            float x_left = ox + col * m.cell_width;
            float x_right = x_left + m.cell_width;

            if (x_left >= ox + clip_w) break;

            float fg_r, fg_g, fg_b;

            if (cell.fg & COLOR_FLAG_DEFAULT) {
                fg_r = def_fg_r; fg_g = def_fg_g; fg_b = def_fg_b;
            } else {
                uint32_t fg_enc = cell.fg;
                if ((cell.attrs & ATTR_BOLD) && !(fg_enc & COLOR_FLAG_TRUECOLOR) && (fg_enc & 0xFF) < 8)
                    fg_enc = (fg_enc & 0xFF) + 8;
                resolve_color(fg_enc, config, fg_r, fg_g, fg_b);
            }

            bool selected = has_selection && buffer.selection.contains(abs_line, col);
            if (selected) {
                fg_r = 1.0f; fg_g = 1.0f; fg_b = 1.0f;
            }

            int mt = has_search ? buffer.search.match_type(abs_line, col) : 0;
            if (mt == 2) {
                fg_r = 0.0f; fg_g = 0.0f; fg_b = 0.0f;
            } else if (mt == 1) {
                fg_r = 0.0f; fg_g = 0.0f; fg_b = 0.0f;
            }

            if (cell.attrs & ATTR_INVERSE) {
                float cell_bg_r, cell_bg_g, cell_bg_b;
                if (cell.bg & COLOR_FLAG_DEFAULT) {
                    cell_bg_r = def_bg_r; cell_bg_g = def_bg_g; cell_bg_b = def_bg_b;
                } else {
                    resolve_color(cell.bg, config, cell_bg_r, cell_bg_g, cell_bg_b);
                }
                fg_r = cell_bg_r; fg_g = cell_bg_g; fg_b = cell_bg_b;
            }

            if (cell.attrs & ATTR_DIM) {
                fg_r *= 0.5f; fg_g *= 0.5f; fg_b *= 0.5f;
            }

            if ((cell.attrs & ATTR_WIDE_CONT) || (cell.codepoint == ' ' && !(cell.attrs & ATTR_UNDERLINE_MASK))) {
                if (cell.codepoint == 0x23F8)
                    dbg("U+23F8: skipped at col=%d attrs=0x%04x (wide_cont or space)", col, cell.attrs);
                continue;
            }

            uint32_t cp = cell.codepoint;
            if (cp == 0x23F8) {
                dbg("U+23F8: attrs=0x%04x wide_cont=%d codepoint=0x%X col=%d",
                    cell.attrs, !!(cell.attrs & ATTR_WIDE_CONT), cell.codepoint, col);
            }
            if (cp >= GRAPHEME_SENTINEL_BASE) continue;

            auto [font_idx, glyph_id] = m_font.find_glyph(cp);
            const GlyphEntry *ge = m_atlas.get(m_font, font_idx, glyph_id);
            if (cp == 0x23F8) {
                dbg("U+23F8: font_idx=%d glyph_id=%u ge=%p ge->w=%d ge->h=%d",
                    font_idx, glyph_id, (void*)ge, ge ? ge->w : -1, ge ? ge->h : -1);
            }
            if (!ge || ge->w == 0) continue;

            float gx, gy, gx2, gy2;
            if (ge->type == GlyphType::Color && ge->h > m.cell_height) {
                // Scale color emoji to fit cell height
                float scale = (float)m.cell_height / ge->h;
                float sw = ge->w * scale;
                float sh = m.cell_height;
                gx = x_left + (m.cell_width - sw) * 0.5f;
                gy = y_top;
                gx2 = gx + sw;
                gy2 = gy + sh;
            } else {
                gx = x_left + ge->bearing_x;
                gy = y_top + m.ascender - ge->bearing_y;
                gx2 = gx + ge->w;
                gy2 = gy + ge->h;
            }

            float tu = ge->u / atlas_size;
            float tv = ge->v / atlas_size;
            float tu2 = (ge->u + ge->w) / atlas_size;
            float tv2 = (ge->v + ge->h) / atlas_size;

            float glyph_type = (ge->type == GlyphType::Color) ? 2.0f : 1.0f;

            m_vertices.push_back({gx,  gy,  tu,  tv,  fg_r, fg_g, fg_b, 1.0f, glyph_type});
            m_vertices.push_back({gx2, gy,  tu2, tv,  fg_r, fg_g, fg_b, 1.0f, glyph_type});
            m_vertices.push_back({gx2, gy2, tu2, tv2, fg_r, fg_g, fg_b, 1.0f, glyph_type});
            m_vertices.push_back({gx,  gy,  tu,  tv,  fg_r, fg_g, fg_b, 1.0f, glyph_type});
            m_vertices.push_back({gx2, gy2, tu2, tv2, fg_r, fg_g, fg_b, 1.0f, glyph_type});
            m_vertices.push_back({gx,  gy2, tu,  tv2, fg_r, fg_g, fg_b, 1.0f, glyph_type});

            if (cell.attrs & ATTR_UNDERLINE_MASK) {
                float uy = y_top + m.ascender + 2;
                float uy2 = uy + std::max(1, m.underline_thickness);
                m_vertices.push_back({x_left,  uy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_right, uy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_right, uy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_left,  uy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_right, uy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_left,  uy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
            }

            if (cell.attrs & ATTR_STRIKETHROUGH) {
                float sy = y_top + m.cell_height / 2.0f;
                float sy2 = sy + 1;
                m_vertices.push_back({x_left,  sy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_right, sy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_right, sy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_left,  sy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_right, sy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                m_vertices.push_back({x_left,  sy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
            }
        }
    }

    // Cursor
    if (buffer.cursor_visible() && buffer.viewport_offset() == 0) {
        int cr = buffer.cursor_row();
        int cc = buffer.cursor_col();
        float cx = ox + cc * m.cell_width;
        float cy = oy + cr * m.cell_height;
        float cur_r = ((config.cursor_color >> 16) & 0xFF) / 255.0f;
        float cur_g = ((config.cursor_color >> 8) & 0xFF) / 255.0f;
        float cur_b = (config.cursor_color & 0xFF) / 255.0f;

        float cw = (float)m.cell_width;
        float ch = (float)m.cell_height;

        if (m_focused && pane_focused) {
            m_vertices.push_back({cx,    cy,    0,0, cur_r,cur_g,cur_b,0.7f, 0});
            m_vertices.push_back({cx+cw, cy,    0,0, cur_r,cur_g,cur_b,0.7f, 0});
            m_vertices.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,0.7f, 0});
            m_vertices.push_back({cx,    cy,    0,0, cur_r,cur_g,cur_b,0.7f, 0});
            m_vertices.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,0.7f, 0});
            m_vertices.push_back({cx,    cy+ch, 0,0, cur_r,cur_g,cur_b,0.7f, 0});
        } else {
            float thick = 2.0f;
            // Top
            m_vertices.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx, cy+thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            // Bottom
            m_vertices.push_back({cx, cy+ch-thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+ch-thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx, cy+ch-thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            // Left
            m_vertices.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+thick, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+thick, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+thick, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            // Right
            m_vertices.push_back({cx+cw-thick, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw-thick, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            m_vertices.push_back({cx+cw-thick, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
        }
    }

    // Search bar overlay (render within pane rect)
    if (buffer.search.focused) {
        float bar_h = m.cell_height + 8;
        float bar_y = oy;
        float bar_w = (float)clip_w;

        float bar_r = 0.15f, bar_g = 0.15f, bar_b = 0.15f;
        m_vertices.push_back({ox,        bar_y,         0,0, bar_r,bar_g,bar_b,0.95f, 0});
        m_vertices.push_back({ox+bar_w,  bar_y,         0,0, bar_r,bar_g,bar_b,0.95f, 0});
        m_vertices.push_back({ox+bar_w,  bar_y + bar_h, 0,0, bar_r,bar_g,bar_b,0.95f, 0});
        m_vertices.push_back({ox,        bar_y,         0,0, bar_r,bar_g,bar_b,0.95f, 0});
        m_vertices.push_back({ox+bar_w,  bar_y + bar_h, 0,0, bar_r,bar_g,bar_b,0.95f, 0});
        m_vertices.push_back({ox,        bar_y + bar_h, 0,0, bar_r,bar_g,bar_b,0.95f, 0});

        float bdr_y = bar_y + bar_h - 1;
        m_vertices.push_back({ox,        bdr_y,     0,0, 0.3f,0.3f,0.3f,1, 0});
        m_vertices.push_back({ox+bar_w,  bdr_y,     0,0, 0.3f,0.3f,0.3f,1, 0});
        m_vertices.push_back({ox+bar_w,  bdr_y + 1, 0,0, 0.3f,0.3f,0.3f,1, 0});
        m_vertices.push_back({ox,        bdr_y,     0,0, 0.3f,0.3f,0.3f,1, 0});
        m_vertices.push_back({ox+bar_w,  bdr_y + 1, 0,0, 0.3f,0.3f,0.3f,1, 0});
        m_vertices.push_back({ox,        bdr_y + 1, 0,0, 0.3f,0.3f,0.3f,1, 0});

        float text_y = bar_y + 4;
        float text_x = ox + 4;

        draw_text(text_x, text_y, "Find: ", 0.6f, 0.6f, 0.6f, atlas_size);
        text_x += 6 * m.cell_width;

        draw_text(text_x, text_y, buffer.search.query, 1.0f, 1.0f, 1.0f, atlas_size);
        text_x += buffer.search.query.size() * m.cell_width;

        float cur_x = text_x;
        m_vertices.push_back({cur_x,   text_y,                     0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        m_vertices.push_back({cur_x+2, text_y,                     0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        m_vertices.push_back({cur_x+2, text_y + (float)m.cell_height, 0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        m_vertices.push_back({cur_x,   text_y,                     0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        m_vertices.push_back({cur_x+2, text_y + (float)m.cell_height, 0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        m_vertices.push_back({cur_x,   text_y + (float)m.cell_height, 0,0, 0.8f,0.8f,0.8f,0.8f, 0});

        if (!buffer.search.query.empty()) {
            std::string info;
            if (buffer.search.total_matches() == 0) {
                info = "No matches";
            } else {
                info = std::to_string(buffer.search.current_match + 1) + " of "
                     + std::to_string(buffer.search.total_matches());
            }
            float info_x = ox + bar_w - (info.size() + 1) * m.cell_width;
            float info_r = buffer.search.total_matches() == 0 ? 0.8f : 0.6f;
            float info_g = buffer.search.total_matches() == 0 ? 0.3f : 0.6f;
            float info_b = buffer.search.total_matches() == 0 ? 0.3f : 0.6f;
            draw_text(info_x, text_y, info, info_r, info_g, info_b, atlas_size);
        }
    }
}

void Renderer::render_dot_grid(int offset_x, int offset_y, int w, int h,
                                int cell_w, int cell_h) {
    if (cell_w <= 0 || cell_h <= 0) return;

    float dot_r = 0.25f, dot_g = 0.25f, dot_b = 0.25f;
    float ds = 2.0f;  // dot size in pixels

    for (int y = offset_y; y < offset_y + h; y += cell_h) {
        for (int x = offset_x; x < offset_x + w; x += cell_w) {
            float cx = x + cell_w * 0.5f - ds * 0.5f;
            float cy = y + cell_h * 0.5f - ds * 0.5f;
            m_vertices.push_back({cx,    cy,    0,0, dot_r,dot_g,dot_b,1, 0});
            m_vertices.push_back({cx+ds, cy,    0,0, dot_r,dot_g,dot_b,1, 0});
            m_vertices.push_back({cx+ds, cy+ds, 0,0, dot_r,dot_g,dot_b,1, 0});
            m_vertices.push_back({cx,    cy,    0,0, dot_r,dot_g,dot_b,1, 0});
            m_vertices.push_back({cx+ds, cy+ds, 0,0, dot_r,dot_g,dot_b,1, 0});
            m_vertices.push_back({cx,    cy+ds, 0,0, dot_r,dot_g,dot_b,1, 0});
        }
    }
}

void Renderer::render_images(ScreenBuffer &buffer, int offset_x, int offset_y,
                              int clip_w, int clip_h) {
    auto &store = buffer.images;
    if (store.placements().empty()) return;

    const auto &m = m_font.metrics();
    int abs_line0 = buffer.absolute_line(0);

    // Set up projection
    float proj[16] = {};
    proj[0] = 2.0f / m_viewport_w;
    proj[5] = -2.0f / m_viewport_h;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = 1.0f;
    proj[15] = 1.0f;

    glUseProgram(m_image_shader);
    int proj_loc = glGetUniformLocation(m_image_shader, "uProjection");
    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj);
    int tex_loc = glGetUniformLocation(m_image_shader, "uImage");
    glUniform1i(tex_loc, 0);

    for (const auto &placement : store.placements()) {
        auto it = store.images().find(placement.image_id);
        if (it == store.images().end()) continue;
        auto &img = it->second;

        store.ensure_texture(img);
        if (!img.gl_texture) continue;

        // Compute screen row from absolute line
        int screen_row = placement.anchor_abs_line - abs_line0;

        // Compute pixel rect
        float px = offset_x + placement.anchor_col * m.cell_width;
        float py = offset_y + screen_row * m.cell_height;
        float pw, ph;

        if (placement.columns > 0)
            pw = placement.columns * m.cell_width;
        else
            pw = (float)img.width;

        if (placement.rows > 0)
            ph = placement.rows * m.cell_height;
        else
            ph = (float)img.height;

        // Clip: skip if entirely off screen
        if (py + ph < offset_y || py >= offset_y + clip_h) continue;
        if (px + pw < offset_x || px >= offset_x + clip_w) continue;

        // Draw textured quad
        float verts[] = {
            px,      py,      0.0f, 0.0f,
            px + pw, py,      1.0f, 0.0f,
            px + pw, py + ph, 1.0f, 1.0f,
            px,      py,      0.0f, 0.0f,
            px + pw, py + ph, 1.0f, 1.0f,
            px,      py + ph, 0.0f, 1.0f,
        };

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, img.gl_texture);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // Disable unused attribs from glyph shader
        glDisableVertexAttribArray(2);
        glDisableVertexAttribArray(3);

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Renderer::render_pane(const ScreenBuffer &buffer, const Config &config,
                            int offset_x, int offset_y, int w, int h,
                            bool pane_focused) {
    // Set scissor to clip rendering to pane rect
    // OpenGL scissor uses bottom-left origin, so convert
    int scissor_y = m_viewport_h - (offset_y + h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(offset_x, scissor_y, w, h);

    // Draw pane background (cells with default bg rely on this rather than glClear,
    // so that the dead zone outside tmux panes stays black)
    float bg_r = ((config.bg_color >> 16) & 0xFF) / 255.0f;
    float bg_g = ((config.bg_color >> 8) & 0xFF) / 255.0f;
    float bg_b = (config.bg_color & 0xFF) / 255.0f;
    float fx = (float)offset_x, fy = (float)offset_y;
    float fw = (float)w, fh = (float)h;
    m_vertices.push_back({fx,    fy,    0,0, bg_r,bg_g,bg_b,1, 0});
    m_vertices.push_back({fx+fw, fy,    0,0, bg_r,bg_g,bg_b,1, 0});
    m_vertices.push_back({fx+fw, fy+fh, 0,0, bg_r,bg_g,bg_b,1, 0});
    m_vertices.push_back({fx,    fy,    0,0, bg_r,bg_g,bg_b,1, 0});
    m_vertices.push_back({fx+fw, fy+fh, 0,0, bg_r,bg_g,bg_b,1, 0});
    m_vertices.push_back({fx,    fy+fh, 0,0, bg_r,bg_g,bg_b,1, 0});

    build_pane_vertices(buffer, config, offset_x, offset_y, w, h, pane_focused);

    // Flush text/glyph vertices for this pane while scissor is active
    flush();

    // Render inline images on top of text
    render_images(const_cast<ScreenBuffer &>(buffer), offset_x, offset_y, w, h);

    glDisable(GL_SCISSOR_TEST);
}

void Renderer::render_border(float x, float y, float w, float h,
                              float r, float g, float b) {
    m_vertices.push_back({x,   y,   0,0, r,g,b,1, 0});
    m_vertices.push_back({x+w, y,   0,0, r,g,b,1, 0});
    m_vertices.push_back({x+w, y+h, 0,0, r,g,b,1, 0});
    m_vertices.push_back({x,   y,   0,0, r,g,b,1, 0});
    m_vertices.push_back({x+w, y+h, 0,0, r,g,b,1, 0});
    m_vertices.push_back({x,   y+h, 0,0, r,g,b,1, 0});
}

// Format a terminal title for display in a tab.
// Strips "user@host:" prefix, abbreviates paths to last 2 components,
// replaces $HOME prefix with ~.
static std::string format_tab_title(const std::string &raw) {
    std::string title = raw;

    // Strip "user@host:" prefix (common bash PROMPT_COMMAND pattern)
    auto colon = title.find(':');
    if (colon != std::string::npos && colon > 0) {
        // Check that everything before colon looks like user@host
        bool looks_like_host = false;
        for (size_t i = 0; i < colon; i++) {
            if (title[i] == '@') { looks_like_host = true; break; }
        }
        if (looks_like_host) {
            title = title.substr(colon + 1);
            // Strip leading space if any
            if (!title.empty() && title[0] == ' ') title = title.substr(1);
        }
    }

    // If it looks like a path, abbreviate to last 2 components
    if (!title.empty() && (title[0] == '/' || title[0] == '~')) {
        // Find the last two path separators
        size_t last = title.rfind('/');
        if (last != std::string::npos && last > 0) {
            size_t prev = title.rfind('/', last - 1);
            if (prev != std::string::npos && prev > 0) {
                // Show .../second-to-last/last
                title = "\xe2\x80\xa6" + title.substr(prev);
            }
        }
    }

    return title;
}

static int utf8_len(const std::string &s) {
    int count = 0;
    for (size_t i = 0; i < s.size(); count++) {
        unsigned char c = s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
    }
    return count;
}

static std::string utf8_truncate(const std::string &s, int max_chars) {
    int count = 0;
    size_t i = 0;
    while (i < s.size() && count < max_chars) {
        unsigned char c = s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        count++;
    }
    return s.substr(0, i);
}

int Renderer::tab_hit_test(const TabManager &tabs, int x) {
    const auto &m = m_font.metrics();
    float tab_x = 4;
    for (int i = 0; i < tabs.tab_count(); i++) {
        const Tab &tab = *tabs.tabs()[i];
        std::string title = tab.title.empty() ? "Terminal" : format_tab_title(tab.title);
        if (utf8_len(title) > 15) title = utf8_truncate(title, 15) + "\xe2\x80\xa6";
        float tab_w = (utf8_len(title) + 2) * m.cell_width;
        if (x >= tab_x && x < tab_x + tab_w)
            return i;
        tab_x += tab_w + 4;
    }
    return -1;
}

void Renderer::render_tab_bar(const TabManager &tabs, const Config &/*config*/, int bar_height) {
    const auto &m = m_font.metrics();
    float atlas_size = (float)m_atlas.texture_size();
    float bar_w = (float)m_viewport_w;

    // Tab bar background
    m_vertices.push_back({0,     0,              0,0, 0.12f,0.12f,0.12f,1, 0});
    m_vertices.push_back({bar_w, 0,              0,0, 0.12f,0.12f,0.12f,1, 0});
    m_vertices.push_back({bar_w, (float)bar_height, 0,0, 0.12f,0.12f,0.12f,1, 0});
    m_vertices.push_back({0,     0,              0,0, 0.12f,0.12f,0.12f,1, 0});
    m_vertices.push_back({bar_w, (float)bar_height, 0,0, 0.12f,0.12f,0.12f,1, 0});
    m_vertices.push_back({0,     (float)bar_height, 0,0, 0.12f,0.12f,0.12f,1, 0});

    // Bottom border
    float bdr_y = (float)bar_height - 1;
    m_vertices.push_back({0,     bdr_y,     0,0, 0.25f,0.25f,0.25f,1, 0});
    m_vertices.push_back({bar_w, bdr_y,     0,0, 0.25f,0.25f,0.25f,1, 0});
    m_vertices.push_back({bar_w, bdr_y + 1, 0,0, 0.25f,0.25f,0.25f,1, 0});
    m_vertices.push_back({0,     bdr_y,     0,0, 0.25f,0.25f,0.25f,1, 0});
    m_vertices.push_back({bar_w, bdr_y + 1, 0,0, 0.25f,0.25f,0.25f,1, 0});
    m_vertices.push_back({0,     bdr_y + 1, 0,0, 0.25f,0.25f,0.25f,1, 0});

    // Render each tab
    float tab_x = 4;
    float text_y = 4;
    int active_idx = tabs.active_index();

    for (int i = 0; i < tabs.tab_count(); i++) {
        const Tab &tab = *tabs.tabs()[i];
        std::string title = tab.title.empty() ? "Terminal" : format_tab_title(tab.title);
        if (utf8_len(title) > 15) title = utf8_truncate(title, 15) + "\xe2\x80\xa6";

        float tab_w = (utf8_len(title) + 2) * m.cell_width;

        if (i == active_idx) {
            // Active tab background
            m_vertices.push_back({tab_x,        0,                  0,0, 0.2f,0.2f,0.2f,1, 0});
            m_vertices.push_back({tab_x+tab_w,  0,                  0,0, 0.2f,0.2f,0.2f,1, 0});
            m_vertices.push_back({tab_x+tab_w,  (float)bar_height-1, 0,0, 0.2f,0.2f,0.2f,1, 0});
            m_vertices.push_back({tab_x,        0,                  0,0, 0.2f,0.2f,0.2f,1, 0});
            m_vertices.push_back({tab_x+tab_w,  (float)bar_height-1, 0,0, 0.2f,0.2f,0.2f,1, 0});
            m_vertices.push_back({tab_x,        (float)bar_height-1, 0,0, 0.2f,0.2f,0.2f,1, 0});

            // Active tab indicator (colored bottom border)
            float ind_y = (float)bar_height - 2;
            m_vertices.push_back({tab_x,       ind_y,            0,0, 0.3f,0.6f,1.0f,1, 0});
            m_vertices.push_back({tab_x+tab_w, ind_y,            0,0, 0.3f,0.6f,1.0f,1, 0});
            m_vertices.push_back({tab_x+tab_w, (float)bar_height, 0,0, 0.3f,0.6f,1.0f,1, 0});
            m_vertices.push_back({tab_x,       ind_y,            0,0, 0.3f,0.6f,1.0f,1, 0});
            m_vertices.push_back({tab_x+tab_w, (float)bar_height, 0,0, 0.3f,0.6f,1.0f,1, 0});
            m_vertices.push_back({tab_x,       (float)bar_height, 0,0, 0.3f,0.6f,1.0f,1, 0});

            draw_text(tab_x + m.cell_width, text_y, title, 0.9f, 0.9f, 0.9f, atlas_size);
        } else {
            // Activity indicator
            if (tab.has_activity) {
                // Small dot
                float dot_x = tab_x + 4;
                float dot_y = text_y + m.cell_height / 2.0f - 2;
                m_vertices.push_back({dot_x,   dot_y,   0,0, 0.3f,0.7f,1.0f,1, 0});
                m_vertices.push_back({dot_x+4, dot_y,   0,0, 0.3f,0.7f,1.0f,1, 0});
                m_vertices.push_back({dot_x+4, dot_y+4, 0,0, 0.3f,0.7f,1.0f,1, 0});
                m_vertices.push_back({dot_x,   dot_y,   0,0, 0.3f,0.7f,1.0f,1, 0});
                m_vertices.push_back({dot_x+4, dot_y+4, 0,0, 0.3f,0.7f,1.0f,1, 0});
                m_vertices.push_back({dot_x,   dot_y+4, 0,0, 0.3f,0.7f,1.0f,1, 0});
            }

            draw_text(tab_x + m.cell_width, text_y, title, 0.5f, 0.5f, 0.5f, atlas_size);
        }

        tab_x += tab_w + 4;
    }
}

void Renderer::flush() {
    if (m_vertices.empty()) return;

    float proj[16] = {};
    proj[0] = 2.0f / m_viewport_w;
    proj[5] = -2.0f / m_viewport_h;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = 1.0f;
    proj[15] = 1.0f;

    glUseProgram(m_glyph_shader);

    int loc = glGetUniformLocation(m_glyph_shader, "uProjection");
    glUniformMatrix4fv(loc, 1, GL_FALSE, proj);

    loc = glGetUniformLocation(m_glyph_shader, "uAtlas");
    glUniform1i(loc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_atlas.texture_id());

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_vertices.size() * sizeof(QuadVertex),
                 m_vertices.data(), GL_DYNAMIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, x));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, u));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, r));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, type));
    glEnableVertexAttribArray(3);

    glDrawArrays(GL_TRIANGLES, 0, (int)m_vertices.size());

    glBindVertexArray(0);
    m_vertices.clear();
}

// Original render method — delegates to build_pane_vertices + flush for backwards compat
void Renderer::render(const ScreenBuffer &buffer, const Config &config) {
    begin_frame(config);
    build_pane_vertices(buffer, config, 0, 0, m_viewport_w, m_viewport_h, true);
    flush();
}

void Renderer::draw_text(float x, float y, const std::string &text,
                         float r, float g, float b, float atlas_size) {
    const auto &m = m_font.metrics();
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp;
        unsigned char c = text[i];
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            if (i + 1 < text.size()) cp = (cp << 6) | (text[i+1] & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            if (i + 1 < text.size()) cp = (cp << 6) | (text[i+1] & 0x3F);
            if (i + 2 < text.size()) cp = (cp << 6) | (text[i+2] & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            if (i + 1 < text.size()) cp = (cp << 6) | (text[i+1] & 0x3F);
            if (i + 2 < text.size()) cp = (cp << 6) | (text[i+2] & 0x3F);
            if (i + 3 < text.size()) cp = (cp << 6) | (text[i+3] & 0x3F);
            i += 4;
        } else {
            cp = '?';
            i += 1;
        }

        auto [font_idx, glyph_id] = m_font.find_glyph(cp);
        const GlyphEntry *ge = m_atlas.get(m_font, font_idx, glyph_id);
        if (ge && ge->w > 0) {
            float gx = x + ge->bearing_x;
            float gy = y + m.ascender - ge->bearing_y;
            float gx2 = gx + ge->w;
            float gy2 = gy + ge->h;

            float tu = ge->u / atlas_size;
            float tv = ge->v / atlas_size;
            float tu2 = (ge->u + ge->w) / atlas_size;
            float tv2 = (ge->v + ge->h) / atlas_size;

            float type = (ge->type == GlyphType::Color) ? 2.0f : 1.0f;
            m_vertices.push_back({gx,  gy,  tu,  tv,  r,g,b,1, type});
            m_vertices.push_back({gx2, gy,  tu2, tv,  r,g,b,1, type});
            m_vertices.push_back({gx2, gy2, tu2, tv2, r,g,b,1, type});
            m_vertices.push_back({gx,  gy,  tu,  tv,  r,g,b,1, type});
            m_vertices.push_back({gx2, gy2, tu2, tv2, r,g,b,1, type});
            m_vertices.push_back({gx,  gy2, tu,  tv2, r,g,b,1, type});
        }
        x += m.cell_width;
    }
}

} // namespace rivt
