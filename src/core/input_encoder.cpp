#include "core/input_encoder.h"
#include "terminal/screen_buffer.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include <cstdio>
#include <cstring>

namespace rivt {

std::string encode_mouse(const MouseEvent &mouse, int cell_col, int cell_row,
                          bool sgr) {
    int cb;
    if (mouse.motion && mouse.button == MouseButton::NoButton) {
        cb = 3 + 32;
    } else if (mouse.motion) {
        switch (mouse.button) {
            case MouseButton::Left:   cb = 0 + 32; break;
            case MouseButton::Middle: cb = 1 + 32; break;
            case MouseButton::Right:  cb = 2 + 32; break;
            default: return {};
        }
    } else if (mouse.button == MouseButton::ScrollUp) {
        cb = 64;
    } else if (mouse.button == MouseButton::ScrollDown) {
        cb = 65;
    } else if (!mouse.pressed && !sgr) {
        cb = 3;
    } else {
        switch (mouse.button) {
            case MouseButton::Left:   cb = 0; break;
            case MouseButton::Middle: cb = 1; break;
            case MouseButton::Right:  cb = 2; break;
            default: return {};
        }
    }

    if (mouse.mods & KeyMod::Shift) cb |= 4;
    if (mouse.mods & KeyMod::Alt)   cb |= 8;
    if (mouse.mods & KeyMod::Ctrl)  cb |= 16;

    int cx = cell_col + 1;
    int cy = cell_row + 1;

    if (sgr) {
        char final_ch = mouse.pressed || mouse.motion ? 'M' : 'm';
        char buf[64];
        snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", cb, cx, cy, final_ch);
        return buf;
    } else {
        if (cx > 223 || cy > 223) return {};
        char buf[6];
        buf[0] = '\033';
        buf[1] = '[';
        buf[2] = 'M';
        buf[3] = (char)(cb + 32);
        buf[4] = (char)(cx + 32);
        buf[5] = (char)(cy + 32);
        return std::string(buf, 6);
    }
}

// Kitty keyboard protocol: map xkb keysym to kitty key number
static int kitty_keycode(uint32_t keysym) {
    switch (keysym) {
        case XKB_KEY_Escape:    return 27;
        case XKB_KEY_Return:    return 13;
        case XKB_KEY_KP_Enter:  return 13;
        case XKB_KEY_Tab:       return 9;
        case XKB_KEY_ISO_Left_Tab: return 9;
        case XKB_KEY_BackSpace: return 127;
        case XKB_KEY_Insert:    return 57348;
        case XKB_KEY_Delete:    return 57349;
        case XKB_KEY_Left:      return 57350;
        case XKB_KEY_Right:     return 57351;
        case XKB_KEY_Up:        return 57352;
        case XKB_KEY_Down:      return 57353;
        case XKB_KEY_Page_Up:   return 57354;
        case XKB_KEY_Page_Down: return 57355;
        case XKB_KEY_Home:      return 57356;
        case XKB_KEY_End:       return 57357;
        case XKB_KEY_Caps_Lock: return 57358;
        case XKB_KEY_Scroll_Lock: return 57359;
        case XKB_KEY_Num_Lock:  return 57360;
        case XKB_KEY_Print:     return 57361;
        case XKB_KEY_Pause:     return 57362;
        case XKB_KEY_Menu:      return 57363;
        case XKB_KEY_F1:  return 57364;
        case XKB_KEY_F2:  return 57365;
        case XKB_KEY_F3:  return 57366;
        case XKB_KEY_F4:  return 57367;
        case XKB_KEY_F5:  return 57368;
        case XKB_KEY_F6:  return 57369;
        case XKB_KEY_F7:  return 57370;
        case XKB_KEY_F8:  return 57371;
        case XKB_KEY_F9:  return 57372;
        case XKB_KEY_F10: return 57373;
        case XKB_KEY_F11: return 57374;
        case XKB_KEY_F12: return 57375;
        case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:     return 57441;
        case XKB_KEY_Control_L: case XKB_KEY_Control_R: return 57442;
        case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:         return 57443;
        case XKB_KEY_Super_L: case XKB_KEY_Super_R:     return 57444;
        default: return 0;
    }
}

static int kitty_modifiers(KeyMod mods) {
    int m = 0;
    if (mods & KeyMod::Shift) m |= 1;
    if (mods & KeyMod::Alt)   m |= 2;
    if (mods & KeyMod::Ctrl)  m |= 4;
    if (mods & KeyMod::Super) m |= 8;
    return m;
}

struct LegacyKey { int num; char final_char; };

static LegacyKey legacy_csi_key(uint32_t keysym) {
    switch (keysym) {
        case XKB_KEY_Up:        return {1, 'A'};
        case XKB_KEY_Down:      return {1, 'B'};
        case XKB_KEY_Right:     return {1, 'C'};
        case XKB_KEY_Left:      return {1, 'D'};
        case XKB_KEY_Home:      return {1, 'H'};
        case XKB_KEY_End:       return {1, 'F'};
        case XKB_KEY_Insert:    return {2, '~'};
        case XKB_KEY_Delete:    return {3, '~'};
        case XKB_KEY_Page_Up:   return {5, '~'};
        case XKB_KEY_Page_Down: return {6, '~'};
        case XKB_KEY_F1:  return {1, 'P'};
        case XKB_KEY_F2:  return {1, 'Q'};
        case XKB_KEY_F3:  return {1, 'R'};
        case XKB_KEY_F4:  return {1, 'S'};
        case XKB_KEY_F5:  return {15, '~'};
        case XKB_KEY_F6:  return {17, '~'};
        case XKB_KEY_F7:  return {18, '~'};
        case XKB_KEY_F8:  return {19, '~'};
        case XKB_KEY_F9:  return {20, '~'};
        case XKB_KEY_F10: return {21, '~'};
        case XKB_KEY_F11: return {23, '~'};
        case XKB_KEY_F12: return {24, '~'};
        default: return {0, 0};
    }
}

std::string encode_key_kitty(const KeyEvent &key, const ScreenBuffer &buffer) {
    if (!key.pressed) return "";

    int flags = buffer.kitty_kbd_flags();
    int mods = kitty_modifiers(key.mods);
    int mod_param = mods + 1;

    LegacyKey lk = legacy_csi_key(key.keysym);
    if (lk.num != 0) {
        char buf[32];
        if (lk.final_char == '~') {
            if (mod_param > 1)
                snprintf(buf, sizeof(buf), "\033[%d;%d~", lk.num, mod_param);
            else
                snprintf(buf, sizeof(buf), "\033[%d~", lk.num);
        } else {
            if (mod_param > 1)
                snprintf(buf, sizeof(buf), "\033[1;%d%c", mod_param, lk.final_char);
            else
                snprintf(buf, sizeof(buf), "\033[%c", lk.final_char);
        }
        return buf;
    }

    int kc = kitty_keycode(key.keysym);

    if (kc == 0) {
        uint32_t sym = key.keysym;
        if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
            kc = sym - XKB_KEY_A + 'a';
        else if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
            kc = sym - XKB_KEY_a + 'a';
        else if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)
            kc = sym - XKB_KEY_0 + '0';
        else if (sym >= XKB_KEY_space && sym <= XKB_KEY_asciitilde)
            kc = sym;
        else if (sym >= 0x100 && sym <= 0x10FFFF)
            kc = sym;
        else
            kc = sym & 0xFFFF;
    }

    if (kc == 0) return "";

    bool need_csi = false;

    if (flags & 1) {
        bool has_significant_mods = mods != 0;
        if (mods == 1 && kc >= 32 && kc < 127 && !key.text.empty())
            has_significant_mods = false;

        if (has_significant_mods)
            need_csi = true;

        if (kc == 27 || kc == 13 || kc == 9 || kc == 127)
            need_csi = true;
    }

    if (flags & 8)
        need_csi = true;

    if (need_csi) {
        char buf[64];
        if (mod_param == 1)
            snprintf(buf, sizeof(buf), "\033[%du", kc);
        else
            snprintf(buf, sizeof(buf), "\033[%d;%du", kc, mod_param);
        return buf;
    }

    if (!key.text.empty())
        return key.text;

    return "";
}

std::string encode_key_legacy(const KeyEvent &key, const ScreenBuffer &buffer) {
    if (!key.pressed) return "";

    bool ctrl  = key.mods & KeyMod::Ctrl;
    bool shift = key.mods & KeyMod::Shift;
    bool alt   = key.mods & KeyMod::Alt;

    std::string seq;
    const char *app = buffer.app_cursor_keys() ? "O" : "[";

    switch (key.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            seq = "\r";
            break;
        case XKB_KEY_BackSpace:
            seq = ctrl ? "\x08" : "\x7f";
            break;
        case XKB_KEY_Tab:
            seq = shift ? "\033[Z" : "\t";
            break;
        case XKB_KEY_ISO_Left_Tab:
            seq = "\033[Z";
            break;
        case XKB_KEY_Escape:
            seq = "\033";
            break;
        case XKB_KEY_Up:
            seq = std::string("\033") + app + "A";
            break;
        case XKB_KEY_Down:
            seq = std::string("\033") + app + "B";
            break;
        case XKB_KEY_Right:
            seq = std::string("\033") + app + "C";
            break;
        case XKB_KEY_Left:
            seq = std::string("\033") + app + "D";
            break;
        case XKB_KEY_Home:
            seq = std::string("\033") + app + "H";
            break;
        case XKB_KEY_End:
            seq = std::string("\033") + app + "F";
            break;
        case XKB_KEY_Insert:
            seq = "\033[2~";
            break;
        case XKB_KEY_Delete:
            seq = "\033[3~";
            break;
        case XKB_KEY_Page_Up:
            if (shift) return "";
            seq = "\033[5~";
            break;
        case XKB_KEY_Page_Down:
            if (shift) return "";
            seq = "\033[6~";
            break;
        case XKB_KEY_F1:  seq = "\033OP"; break;
        case XKB_KEY_F2:  seq = "\033OQ"; break;
        case XKB_KEY_F3:  seq = "\033OR"; break;
        case XKB_KEY_F4:  seq = "\033OS"; break;
        case XKB_KEY_F5:  seq = "\033[15~"; break;
        case XKB_KEY_F6:  seq = "\033[17~"; break;
        case XKB_KEY_F7:  seq = "\033[18~"; break;
        case XKB_KEY_F8:  seq = "\033[19~"; break;
        case XKB_KEY_F9:  seq = "\033[20~"; break;
        case XKB_KEY_F10: seq = "\033[21~"; break;
        case XKB_KEY_F11: seq = "\033[23~"; break;
        case XKB_KEY_F12: seq = "\033[24~"; break;
        default:
            if (ctrl) {
                // Ctrl + punctuation/symbol control codes
                switch (key.keysym) {
                    case XKB_KEY_space:
                    case XKB_KEY_at:
                        seq = std::string(1, '\0');
                        break;
                    case XKB_KEY_bracketleft:
                        seq = "\033";
                        break;
                    case XKB_KEY_backslash:
                        seq = std::string(1, '\x1c');
                        break;
                    case XKB_KEY_bracketright:
                        seq = std::string(1, '\x1d');
                        break;
                    case XKB_KEY_asciicircum:
                        seq = std::string(1, '\x1e');
                        break;
                    case XKB_KEY_underscore:
                    case XKB_KEY_slash:
                        seq = std::string(1, '\x1f');
                        break;
                    default:
                        break;
                }
                if (!seq.empty()) break;

                if (!key.text.empty() && key.text.size() == 1) {
                    char c = key.text[0];
                    if (c >= 'a' && c <= 'z') {
                        seq = std::string(1, (char)(c - 'a' + 1));
                        break;
                    }
                    if (c >= 'A' && c <= 'Z') {
                        seq = std::string(1, (char)(c - 'A' + 1));
                        break;
                    }
                }
            }
            if (seq.empty() && !key.text.empty()) {
                seq = key.text;
            }
            break;
    }

    if (!seq.empty() && alt) {
        seq = "\033" + seq;
    }

    return seq;
}

std::string encode_key(const KeyEvent &key, const ScreenBuffer &buffer) {
    if (buffer.kitty_kbd_active())
        return encode_key_kitty(key, buffer);
    return encode_key_legacy(key, buffer);
}

} // namespace rivt
