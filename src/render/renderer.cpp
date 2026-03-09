#define GL_GLEXT_PROTOTYPES
#include "render/renderer.h"
#include "terminal/screen_buffer.h"
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
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (bg_shader_) glDeleteProgram(bg_shader_);
    if (glyph_shader_) glDeleteProgram(glyph_shader_);
}

bool Renderer::init(const Config &config) {
    if (!font_.init(config.font_family, config.font_size))
        return false;

    if (!atlas_.init(1024))
        return false;

    build_shaders();

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    return true;
}

void Renderer::build_shaders() {
    // We use a unified shader for simplicity
    auto vert = compile_shader(GL_VERTEX_SHADER, glyph_vertex_src);
    auto frag = compile_shader(GL_FRAGMENT_SHADER, glyph_fragment_src);
    glyph_shader_ = link_program(vert, frag);
}

void Renderer::set_viewport(int width, int height) {
    viewport_w_ = width;
    viewport_h_ = height;
    glViewport(0, 0, width, height);
}

void Renderer::compute_grid(int pixel_w, int pixel_h, int &cols, int &rows) const {
    const auto &m = font_.metrics();
    cols = pixel_w / m.cell_width;
    rows = pixel_h / m.cell_height;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
}

void Renderer::resolve_color(uint32_t encoded, const Config &config,
                              float &r, float &g, float &b) const {
    if (encoded & COLOR_FLAG_DEFAULT) {
        // Will be resolved by caller (fg vs bg)
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

void Renderer::render(const ScreenBuffer &buffer, const Config &config) {
    // Clear with background color
    float bgr = ((config.bg_color >> 16) & 0xFF) / 255.0f;
    float bgg = ((config.bg_color >> 8) & 0xFF) / 255.0f;
    float bgb = (config.bg_color & 0xFF) / 255.0f;
    glClearColor(bgr, bgg, bgb, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    vertices_.clear();

    // Build projection matrix (orthographic, pixel coords, origin top-left)
    float proj[16] = {};
    proj[0] = 2.0f / viewport_w_;
    proj[5] = -2.0f / viewport_h_;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = 1.0f;
    proj[15] = 1.0f;

    const auto &m = font_.metrics();
    float def_fg_r = ((config.fg_color >> 16) & 0xFF) / 255.0f;
    float def_fg_g = ((config.fg_color >> 8) & 0xFF) / 255.0f;
    float def_fg_b = (config.fg_color & 0xFF) / 255.0f;
    float def_bg_r = bgr, def_bg_g = bgg, def_bg_b = bgb;

    float atlas_size = (float)atlas_.texture_size();

    // Iterate over visible cells
    for (int row = 0; row < buffer.rows(); row++) {
        const Line &line = buffer.line(row);
        float y_top = row * m.cell_height;

        for (int col = 0; col < buffer.cols() && col < (int)line.cells.size(); col++) {
            const Cell &cell = line.cells[col];
            float x_left = col * m.cell_width;
            float x_right = x_left + m.cell_width;
            float y_bottom = y_top + m.cell_height;

            // Resolve colors
            float fg_r, fg_g, fg_b, cell_bg_r, cell_bg_g, cell_bg_b;

            if (cell.fg & COLOR_FLAG_DEFAULT) {
                fg_r = def_fg_r; fg_g = def_fg_g; fg_b = def_fg_b;
            } else {
                resolve_color(cell.fg, config, fg_r, fg_g, fg_b);
            }

            if (cell.bg & COLOR_FLAG_DEFAULT) {
                cell_bg_r = def_bg_r; cell_bg_g = def_bg_g; cell_bg_b = def_bg_b;
            } else {
                resolve_color(cell.bg, config, cell_bg_r, cell_bg_g, cell_bg_b);
            }

            // Selection: use selection_color as background, white as foreground
            int abs_line = buffer.absolute_line(row);
            bool selected = buffer.selection.contains(abs_line, col);
            if (selected) {
                cell_bg_r = ((config.selection_color >> 16) & 0xFF) / 255.0f;
                cell_bg_g = ((config.selection_color >> 8) & 0xFF) / 255.0f;
                cell_bg_b = (config.selection_color & 0xFF) / 255.0f;
                fg_r = 1.0f; fg_g = 1.0f; fg_b = 1.0f;
            }

            // Search highlight
            int mt = buffer.search.match_type(abs_line, col);
            if (mt == 2) {
                // Current match: bright orange
                cell_bg_r = 0.9f; cell_bg_g = 0.6f; cell_bg_b = 0.1f;
                fg_r = 0.0f; fg_g = 0.0f; fg_b = 0.0f;
            } else if (mt == 1) {
                // Other matches: yellow
                cell_bg_r = 0.6f; cell_bg_g = 0.5f; cell_bg_b = 0.1f;
                fg_r = 0.0f; fg_g = 0.0f; fg_b = 0.0f;
            }

            // Handle inverse
            if (cell.attrs & ATTR_INVERSE) {
                std::swap(fg_r, cell_bg_r);
                std::swap(fg_g, cell_bg_g);
                std::swap(fg_b, cell_bg_b);
            }

            // Handle dim
            if (cell.attrs & ATTR_DIM) {
                fg_r *= 0.5f; fg_g *= 0.5f; fg_b *= 0.5f;
            }

            // Background quad (only if non-default, inverse, selected, or search match)
            if (!(cell.bg & COLOR_FLAG_DEFAULT) || (cell.attrs & ATTR_INVERSE) || selected || mt) {
                // Triangle 1
                vertices_.push_back({x_left,  y_top,    0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                vertices_.push_back({x_right, y_top,    0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                vertices_.push_back({x_right, y_bottom, 0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                // Triangle 2
                vertices_.push_back({x_left,  y_top,    0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                vertices_.push_back({x_right, y_bottom, 0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
                vertices_.push_back({x_left,  y_bottom, 0, 0, cell_bg_r, cell_bg_g, cell_bg_b, 1.0f, 0});
            }

            // Skip if continuation of wide char or space
            if ((cell.attrs & ATTR_WIDE_CONT) || (cell.codepoint == ' ' && !(cell.attrs & ATTR_UNDERLINE_MASK)))
                continue;

            // Find and render glyph
            uint32_t cp = cell.codepoint;
            if (cp >= GRAPHEME_SENTINEL_BASE) continue; // TODO: grapheme cluster support

            auto [font_idx, glyph_id] = font_.find_glyph(cp);
            const GlyphEntry *ge = atlas_.get(font_, font_idx, glyph_id);
            if (!ge || ge->w == 0) continue;

            float gx = x_left + ge->bearing_x;
            float gy = y_top + m.ascender - ge->bearing_y;
            float gx2 = gx + ge->w;
            float gy2 = gy + ge->h;

            float tu = ge->u / atlas_size;
            float tv = ge->v / atlas_size;
            float tu2 = (ge->u + ge->w) / atlas_size;
            float tv2 = (ge->v + ge->h) / atlas_size;

            float glyph_type = (ge->type == GlyphType::Color) ? 2.0f : 1.0f;

            // Triangle 1
            vertices_.push_back({gx,  gy,  tu,  tv,  fg_r, fg_g, fg_b, 1.0f, glyph_type});
            vertices_.push_back({gx2, gy,  tu2, tv,  fg_r, fg_g, fg_b, 1.0f, glyph_type});
            vertices_.push_back({gx2, gy2, tu2, tv2, fg_r, fg_g, fg_b, 1.0f, glyph_type});
            // Triangle 2
            vertices_.push_back({gx,  gy,  tu,  tv,  fg_r, fg_g, fg_b, 1.0f, glyph_type});
            vertices_.push_back({gx2, gy2, tu2, tv2, fg_r, fg_g, fg_b, 1.0f, glyph_type});
            vertices_.push_back({gx,  gy2, tu,  tv2, fg_r, fg_g, fg_b, 1.0f, glyph_type});

            // Underline
            if (cell.attrs & ATTR_UNDERLINE_MASK) {
                float uy = y_top + m.ascender + 2;
                float uy2 = uy + std::max(1, m.underline_thickness);
                vertices_.push_back({x_left,  uy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_right, uy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_right, uy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_left,  uy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_right, uy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_left,  uy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
            }

            // Strikethrough
            if (cell.attrs & ATTR_STRIKETHROUGH) {
                float sy = y_top + m.cell_height / 2.0f;
                float sy2 = sy + 1;
                vertices_.push_back({x_left,  sy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_right, sy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_right, sy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_left,  sy,  0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_right, sy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
                vertices_.push_back({x_left,  sy2, 0, 0, fg_r, fg_g, fg_b, 1.0f, 0});
            }
        }
    }

    // Render cursor
    if (buffer.cursor_visible() && buffer.viewport_offset() == 0) {
        int cr = buffer.cursor_row();
        int cc = buffer.cursor_col();
        float cx = cc * m.cell_width;
        float cy = cr * m.cell_height;
        float cur_r = ((config.cursor_color >> 16) & 0xFF) / 255.0f;
        float cur_g = ((config.cursor_color >> 8) & 0xFF) / 255.0f;
        float cur_b = (config.cursor_color & 0xFF) / 255.0f;

        float cw = (float)m.cell_width;
        float ch = (float)m.cell_height;

        if (focused_) {
            // Filled block cursor
            vertices_.push_back({cx,    cy,    0,0, cur_r,cur_g,cur_b,0.7f, 0});
            vertices_.push_back({cx+cw, cy,    0,0, cur_r,cur_g,cur_b,0.7f, 0});
            vertices_.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,0.7f, 0});
            vertices_.push_back({cx,    cy,    0,0, cur_r,cur_g,cur_b,0.7f, 0});
            vertices_.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,0.7f, 0});
            vertices_.push_back({cx,    cy+ch, 0,0, cur_r,cur_g,cur_b,0.7f, 0});
        } else {
            // Hollow outline cursor
            float thick = 2.0f;
            // Top
            vertices_.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx, cy+thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            // Bottom
            vertices_.push_back({cx, cy+ch-thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+ch-thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx, cy+ch-thick, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            // Left
            vertices_.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+thick, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+thick, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+thick, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            // Right
            vertices_.push_back({cx+cw-thick, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw-thick, cy, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
            vertices_.push_back({cx+cw-thick, cy+ch, 0,0, cur_r,cur_g,cur_b,1, 0});
        }
    }

    // Search bar overlay (only when focused)
    if (buffer.search.focused) {
        float bar_h = m.cell_height + 8;
        float bar_y = 0;
        float bar_w = (float)viewport_w_;

        // Bar background (dark gray)
        float bar_r = 0.15f, bar_g = 0.15f, bar_b = 0.15f;
        vertices_.push_back({0,     bar_y,         0,0, bar_r,bar_g,bar_b,0.95f, 0});
        vertices_.push_back({bar_w, bar_y,         0,0, bar_r,bar_g,bar_b,0.95f, 0});
        vertices_.push_back({bar_w, bar_y + bar_h, 0,0, bar_r,bar_g,bar_b,0.95f, 0});
        vertices_.push_back({0,     bar_y,         0,0, bar_r,bar_g,bar_b,0.95f, 0});
        vertices_.push_back({bar_w, bar_y + bar_h, 0,0, bar_r,bar_g,bar_b,0.95f, 0});
        vertices_.push_back({0,     bar_y + bar_h, 0,0, bar_r,bar_g,bar_b,0.95f, 0});

        // Bottom border (subtle highlight)
        float bdr_y = bar_y + bar_h - 1;
        vertices_.push_back({0,     bdr_y,     0,0, 0.3f,0.3f,0.3f,1, 0});
        vertices_.push_back({bar_w, bdr_y,     0,0, 0.3f,0.3f,0.3f,1, 0});
        vertices_.push_back({bar_w, bdr_y + 1, 0,0, 0.3f,0.3f,0.3f,1, 0});
        vertices_.push_back({0,     bdr_y,     0,0, 0.3f,0.3f,0.3f,1, 0});
        vertices_.push_back({bar_w, bdr_y + 1, 0,0, 0.3f,0.3f,0.3f,1, 0});
        vertices_.push_back({0,     bdr_y + 1, 0,0, 0.3f,0.3f,0.3f,1, 0});

        float text_y = bar_y + 4;
        float text_x = 4;

        // "Find: " label
        draw_text(text_x, text_y, "Find: ", 0.6f, 0.6f, 0.6f, atlas_size);
        text_x += 6 * m.cell_width;

        // Query text
        draw_text(text_x, text_y, buffer.search.query, 1.0f, 1.0f, 1.0f, atlas_size);
        text_x += buffer.search.query.size() * m.cell_width;

        // Cursor
        float cur_x = text_x;
        vertices_.push_back({cur_x,   text_y,                     0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        vertices_.push_back({cur_x+2, text_y,                     0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        vertices_.push_back({cur_x+2, text_y + (float)m.cell_height, 0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        vertices_.push_back({cur_x,   text_y,                     0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        vertices_.push_back({cur_x+2, text_y + (float)m.cell_height, 0,0, 0.8f,0.8f,0.8f,0.8f, 0});
        vertices_.push_back({cur_x,   text_y + (float)m.cell_height, 0,0, 0.8f,0.8f,0.8f,0.8f, 0});

        // Match count (right-aligned)
        if (!buffer.search.query.empty()) {
            std::string info;
            if (buffer.search.total_matches() == 0) {
                info = "No matches";
            } else {
                info = std::to_string(buffer.search.current_match + 1) + " of "
                     + std::to_string(buffer.search.total_matches());
            }
            float info_x = bar_w - (info.size() + 1) * m.cell_width;
            float info_r = buffer.search.total_matches() == 0 ? 0.8f : 0.6f;
            float info_g = buffer.search.total_matches() == 0 ? 0.3f : 0.6f;
            float info_b = buffer.search.total_matches() == 0 ? 0.3f : 0.6f;
            draw_text(info_x, text_y, info, info_r, info_g, info_b, atlas_size);
        }
    }

    if (vertices_.empty()) return;

    // Upload and draw
    glUseProgram(glyph_shader_);

    int loc = glGetUniformLocation(glyph_shader_, "uProjection");
    glUniformMatrix4fv(loc, 1, GL_FALSE, proj);

    loc = glGetUniformLocation(glyph_shader_, "uAtlas");
    glUniform1i(loc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_.texture_id());

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertices_.size() * sizeof(QuadVertex),
                 vertices_.data(), GL_DYNAMIC_DRAW);

    // aPos
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, x));
    glEnableVertexAttribArray(0);
    // aTexCoord
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, u));
    glEnableVertexAttribArray(1);
    // aColor
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, r));
    glEnableVertexAttribArray(2);
    // aType
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(QuadVertex),
                          (void *)offsetof(QuadVertex, type));
    glEnableVertexAttribArray(3);

    glDrawArrays(GL_TRIANGLES, 0, (int)vertices_.size());

    glBindVertexArray(0);
}

void Renderer::draw_text(float x, float y, const std::string &text,
                         float r, float g, float b, float atlas_size) {
    const auto &m = font_.metrics();
    for (unsigned char ch : text) {
        auto [font_idx, glyph_id] = font_.find_glyph(ch);
        const GlyphEntry *ge = atlas_.get(font_, font_idx, glyph_id);
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
            vertices_.push_back({gx,  gy,  tu,  tv,  r,g,b,1, type});
            vertices_.push_back({gx2, gy,  tu2, tv,  r,g,b,1, type});
            vertices_.push_back({gx2, gy2, tu2, tv2, r,g,b,1, type});
            vertices_.push_back({gx,  gy,  tu,  tv,  r,g,b,1, type});
            vertices_.push_back({gx2, gy2, tu2, tv2, r,g,b,1, type});
            vertices_.push_back({gx,  gy2, tu,  tv2, r,g,b,1, type});
        }
        x += m.cell_width;
    }
}

} // namespace rivt
