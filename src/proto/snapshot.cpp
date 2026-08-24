#include "proto/snapshot.h"
#include "proto/wire.h"
#include "terminal/screen_buffer.h"
#include "terminal/vt_parser.h"

namespace rivt::proto {

static constexpr uint32_t SNAP_MAGIC = 0x504E5352;  // "RSNP"
static constexpr uint8_t SNAP_VERSION = 1;

// Section tags. Unknown tags are skipped on read, so sections can be
// added without breaking older readers within the same version.
enum : uint8_t {
    SEC_GEOM = 1,
    SEC_GRID_MAIN = 2,
    SEC_GRID_ALT = 3,
    SEC_SCROLLBACK = 4,
    SEC_CURSOR = 5,
    SEC_SGR = 6,
    SEC_MODES = 7,
    SEC_CHARSET = 8,
    SEC_KITTY = 9,
    SEC_HYPERLINKS = 10,
    SEC_PARSER = 11,
};

// Mode bitmask (SEC_MODES)
enum : uint32_t {
    MODE_APP_CURSOR = 1u << 0,
    MODE_CURSOR_BLINK = 1u << 1,
    MODE_FOCUS_EVENTS = 1u << 2,
    MODE_BRACKETED_PASTE = 1u << 3,
    MODE_SGR_MOUSE = 1u << 4,
    MODE_GRAPHEME_CLUSTER = 1u << 5,
    MODE_SYNCHRONIZED_UPDATE = 1u << 6,
    MODE_CURSOR_VISIBLE = 1u << 7,
};

void Snapshot::encode_line(Writer &w, const Line &l) {
    w.u8(l.wrapped ? 1 : 0);
    w.u32(l.semantic_zone);
    w.u16((uint16_t)l.cells.size());
    size_t i = 0;
    while (i < l.cells.size()) {
        const Cell &c = l.cells[i];
        size_t run = 1;
        while (i + run < l.cells.size()) {
            const Cell &d = l.cells[i + run];
            if (d.codepoint != c.codepoint || d.fg != c.fg || d.bg != c.bg ||
                d.attrs != c.attrs || d.hyperlink_id != c.hyperlink_id)
                break;
            run++;
        }
        w.u16((uint16_t)run);
        w.u32(c.codepoint);
        w.u32(c.fg);
        w.u32(c.bg);
        w.u16(c.attrs);
        w.u16(c.hyperlink_id);
        i += run;
    }
}

bool Snapshot::decode_line(Reader &r, Line &l) {
    l.wrapped = r.u8() != 0;
    l.semantic_zone = r.u32();
    uint16_t ncells = r.u16();
    if (!r.ok || ncells > 4096) return false;
    l.cells.clear();
    l.cells.reserve(ncells);
    while (l.cells.size() < ncells) {
        uint16_t run = r.u16();
        Cell c;
        c.codepoint = r.u32();
        c.fg = r.u32();
        c.bg = r.u32();
        c.attrs = r.u16();
        c.hyperlink_id = r.u16();
        if (!r.ok || run == 0 || l.cells.size() + run > ncells) return false;
        l.cells.insert(l.cells.end(), run, c);
    }
    l.dirty = true;
    return r.ok;
}

static void append_section(Writer &out, uint8_t tag, const Writer &payload) {
    out.u8(tag);
    out.u32((uint32_t)payload.buf.size());
    out.bytes(payload.buf.data(), payload.buf.size());
}

std::vector<uint8_t> Snapshot::serialize(const ScreenBuffer &sb, const VtParser &vp,
                                         int max_scrollback) {
    Writer out;
    out.u32(SNAP_MAGIC);
    out.u8(SNAP_VERSION);

    {
        Writer w;
        w.u16((uint16_t)sb.m_cols);
        w.u16((uint16_t)sb.m_rows);
        w.u32((uint32_t)sb.m_scrollback_limit);
        w.u8(sb.m_using_alt_screen ? 1 : 0);
        append_section(out, SEC_GEOM, w);
    }
    {
        Writer w;
        for (int r = 0; r < sb.m_rows; r++) Snapshot::encode_line(w, sb.sline(r));
        append_section(out, SEC_GRID_MAIN, w);
    }
    {
        Writer w;
        w.u16((uint16_t)sb.m_alt_screen.size());
        for (const Line &l : sb.m_alt_screen) Snapshot::encode_line(w, l);
        append_section(out, SEC_GRID_ALT, w);
    }
    {
        Writer w;
        size_t total = sb.m_scrollback.size();
        size_t include = (max_scrollback < 0 || (size_t)max_scrollback >= total)
                             ? total
                             : (size_t)max_scrollback;
        // Older lines not included are reported so the client can
        // account for (and later fetch) the rest of the history.
        w.u32((uint32_t)include);
        w.u32((uint32_t)(total - include + sb.m_scrollback_trimmed));
        for (size_t i = total - include; i < total; i++) Snapshot::encode_line(w, sb.m_scrollback[i]);
        append_section(out, SEC_SCROLLBACK, w);
    }
    {
        Writer w;
        w.u16((uint16_t)sb.m_cursor_row);
        w.u16((uint16_t)sb.m_cursor_col);
        w.u16((uint16_t)sb.m_saved_cursor.row);
        w.u16((uint16_t)sb.m_saved_cursor.col);
        w.u32(sb.m_saved_cursor.fg);
        w.u32(sb.m_saved_cursor.bg);
        w.u16(sb.m_saved_cursor.attrs);
        w.u8((uint8_t)sb.m_saved_cursor.charset_g0);
        w.u8((uint8_t)sb.m_saved_cursor.charset_g1);
        w.u8((uint8_t)sb.m_saved_cursor.gl_charset);
        w.u16((uint16_t)sb.m_scroll_top);
        w.u16((uint16_t)sb.m_scroll_bottom);
        append_section(out, SEC_CURSOR, w);
    }
    {
        Writer w;
        w.u32(sb.m_cur_fg);
        w.u32(sb.m_bg);
        w.u16(sb.m_cur_attrs);
        w.u32(sb.m_last_printed);
        append_section(out, SEC_SGR, w);
    }
    {
        Writer w;
        uint32_t m = 0;
        if (sb.m_mode_app_cursor) m |= MODE_APP_CURSOR;
        if (sb.m_mode_cursor_blink) m |= MODE_CURSOR_BLINK;
        if (sb.m_mode_focus_events) m |= MODE_FOCUS_EVENTS;
        if (sb.m_mode_bracketed_paste) m |= MODE_BRACKETED_PASTE;
        if (sb.m_mode_sgr_mouse) m |= MODE_SGR_MOUSE;
        if (sb.m_mode_grapheme_cluster) m |= MODE_GRAPHEME_CLUSTER;
        if (sb.m_mode_synchronized_update) m |= MODE_SYNCHRONIZED_UPDATE;
        if (sb.m_cursor_visible) m |= MODE_CURSOR_VISIBLE;
        w.u32(m);
        w.u16((uint16_t)sb.m_mouse_mode);
        append_section(out, SEC_MODES, w);
    }
    {
        Writer w;
        w.u8((uint8_t)sb.m_charset_g0);
        w.u8((uint8_t)sb.m_charset_g1);
        w.u8((uint8_t)sb.m_gl_charset);
        append_section(out, SEC_CHARSET, w);
    }
    {
        Writer w;
        w.u16((uint16_t)sb.m_kitty_kbd_stack.size());
        for (int f : sb.m_kitty_kbd_stack) w.i32(f);
        w.u16((uint16_t)sb.m_saved_kitty_kbd_stack.size());
        for (int f : sb.m_saved_kitty_kbd_stack) w.i32(f);
        append_section(out, SEC_KITTY, w);
    }
    {
        Writer w;
        w.u16(sb.m_cur_hyperlink_id);
        w.u16(sb.m_next_hyperlink_id);
        w.u32((uint32_t)sb.m_hyperlinks.size());
        for (const auto &[id, uri] : sb.m_hyperlinks) {
            w.u16(id);
            w.str(uri);
        }
        append_section(out, SEC_HYPERLINKS, w);
    }
    {
        Writer w;
        w.u8((uint8_t)vp.m_state);
        w.u32(vp.m_utf8_codepoint);
        w.u8((uint8_t)vp.m_utf8_remaining);
        w.str(vp.m_csi_param_str);
        w.u8((uint8_t)vp.m_csi_intermediate);
        w.u8((uint8_t)vp.m_esc_intermediate);
        w.str(vp.m_osc_string);
        w.str(vp.m_apc_string);
        w.str(vp.m_dcs_param_str);
        append_section(out, SEC_PARSER, w);
    }
    return std::move(out.buf);
}

bool Snapshot::deserialize(ScreenBuffer &sb, VtParser &vp,
                           const uint8_t *data, size_t len) {
    Reader top(data, len);
    if (top.u32() != SNAP_MAGIC || top.u8() != SNAP_VERSION || !top.ok) return false;

    bool got_geom = false;
    while (top.ok && top.remaining() > 0) {
        uint8_t tag = top.u8();
        uint32_t slen = top.u32();
        if (!top.ok || top.remaining() < slen) return false;
        Reader r(top.p, slen);
        top.skip(slen);

        switch (tag) {
        case SEC_GEOM: {
            int cols = r.u16(), rows = r.u16();
            int limit = (int)r.u32();
            bool alt = r.u8() != 0;
            if (!r.ok || cols < 1 || rows < 1 || cols > 4096 || rows > 4096) return false;
            sb.m_cols = cols;
            sb.m_rows = rows;
            sb.m_scrollback_limit = limit;
            sb.m_using_alt_screen = alt;
            got_geom = true;
            break;
        }
        case SEC_GRID_MAIN: {
            if (!got_geom) return false;
            std::vector<Line> grid(sb.m_rows, Line(sb.m_cols));
            for (Line &l : grid)
                if (!Snapshot::decode_line(r, l)) return false;
            sb.m_screen = std::move(grid);
            sb.m_screen_top = 0;
            break;
        }
        case SEC_GRID_ALT: {
            uint16_t n = r.u16();
            if (!r.ok || n > 4096) return false;
            std::vector<Line> grid(n, Line(got_geom ? sb.m_cols : 80));
            for (Line &l : grid)
                if (!Snapshot::decode_line(r, l)) return false;
            sb.m_alt_screen = std::move(grid);
            break;
        }
        case SEC_SCROLLBACK: {
            uint32_t n = r.u32();
            uint32_t omitted = r.u32();
            if (!r.ok || n > 1000000) return false;
            sb.m_scrollback.clear();
            for (uint32_t i = 0; i < n; i++) {
                Line l(got_geom ? sb.m_cols : 80);
                if (!Snapshot::decode_line(r, l)) return false;
                sb.m_scrollback.push_back(std::move(l));
            }
            sb.m_scrollback_trimmed = (int)omitted;
            break;
        }
        case SEC_CURSOR: {
            sb.m_cursor_row = r.u16();
            sb.m_cursor_col = r.u16();
            sb.m_saved_cursor.row = r.u16();
            sb.m_saved_cursor.col = r.u16();
            sb.m_saved_cursor.fg = r.u32();
            sb.m_saved_cursor.bg = r.u32();
            sb.m_saved_cursor.attrs = r.u16();
            sb.m_saved_cursor.charset_g0 = r.u8();
            sb.m_saved_cursor.charset_g1 = r.u8();
            sb.m_saved_cursor.gl_charset = r.u8();
            sb.m_scroll_top = r.u16();
            sb.m_scroll_bottom = r.u16();
            break;
        }
        case SEC_SGR: {
            sb.m_cur_fg = r.u32();
            sb.m_bg = r.u32();
            sb.m_cur_attrs = r.u16();
            sb.m_last_printed = r.u32();
            break;
        }
        case SEC_MODES: {
            uint32_t m = r.u32();
            sb.m_mode_app_cursor = m & MODE_APP_CURSOR;
            sb.m_mode_cursor_blink = m & MODE_CURSOR_BLINK;
            sb.m_mode_focus_events = m & MODE_FOCUS_EVENTS;
            sb.m_mode_bracketed_paste = m & MODE_BRACKETED_PASTE;
            sb.m_mode_sgr_mouse = m & MODE_SGR_MOUSE;
            sb.m_mode_grapheme_cluster = m & MODE_GRAPHEME_CLUSTER;
            sb.m_mode_synchronized_update = m & MODE_SYNCHRONIZED_UPDATE;
            sb.m_cursor_visible = m & MODE_CURSOR_VISIBLE;
            sb.m_mouse_mode = r.u16();
            break;
        }
        case SEC_CHARSET: {
            sb.m_charset_g0 = r.u8();
            sb.m_charset_g1 = r.u8();
            sb.m_gl_charset = r.u8();
            break;
        }
        case SEC_KITTY: {
            uint16_t n = r.u16();
            if (!r.ok || n > 1024) return false;
            sb.m_kitty_kbd_stack.clear();
            for (uint16_t i = 0; i < n; i++) sb.m_kitty_kbd_stack.push_back(r.i32());
            n = r.u16();
            if (!r.ok || n > 1024) return false;
            sb.m_saved_kitty_kbd_stack.clear();
            for (uint16_t i = 0; i < n; i++) sb.m_saved_kitty_kbd_stack.push_back(r.i32());
            break;
        }
        case SEC_HYPERLINKS: {
            sb.m_cur_hyperlink_id = r.u16();
            sb.m_next_hyperlink_id = r.u16();
            uint32_t n = r.u32();
            if (!r.ok || n > 65536) return false;
            sb.m_hyperlinks.clear();
            for (uint32_t i = 0; i < n; i++) {
                uint16_t id = r.u16();
                sb.m_hyperlinks[id] = r.str();
            }
            break;
        }
        case SEC_PARSER: {
            vp.m_state = (VtParser::State)r.u8();
            vp.m_utf8_codepoint = r.u32();
            vp.m_utf8_remaining = r.u8();
            vp.m_csi_param_str = r.str();
            vp.m_csi_intermediate = (char)r.u8();
            vp.m_esc_intermediate = (char)r.u8();
            vp.m_osc_string = r.str();
            vp.m_apc_string = r.str();
            vp.m_dcs_param_str = r.str();
            break;
        }
        default:
            break;  // unknown section: skip (already consumed)
        }
        if (!r.ok) return false;
    }
    if (!top.ok || !got_geom) return false;

    // Clamp cursor and scroll region against the restored geometry, and
    // reset client-view state that snapshots deliberately exclude.
    if (sb.m_cursor_row >= sb.m_rows) sb.m_cursor_row = sb.m_rows - 1;
    if (sb.m_cursor_col > sb.m_cols) sb.m_cursor_col = sb.m_cols;
    if (sb.m_scroll_bottom >= sb.m_rows || sb.m_scroll_bottom < sb.m_scroll_top)
        sb.m_scroll_bottom = sb.m_rows - 1;
    if (sb.m_scroll_top < 0 || sb.m_scroll_top > sb.m_scroll_bottom) sb.m_scroll_top = 0;
    sb.m_viewport_offset = 0;
    sb.selection = Selection{};
    sb.search = SearchState{};
    return true;
}

} // namespace rivt::proto
