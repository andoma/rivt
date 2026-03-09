#include "platform/platform.h"
#include "core/event_loop.h"
#include "core/config.h"
#include "terminal/vt_parser.h"
#include "terminal/screen_buffer.h"
#include "render/renderer.h"
#include "pty/pty.h"

#include <xkbcommon/xkbcommon-keysyms.h>
#include <cstdio>
#include <cstring>
#include <signal.h>

using namespace rivt;

static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int) {
    got_sigchld = 1;
}

// Kitty keyboard protocol: map xkb keysym to kitty key number
// Returns 0 if not a special key (use Unicode codepoint instead)
static int kitty_keycode(uint32_t keysym) {
    switch (keysym) {
        case XKB_KEY_Escape:    return 27;
        case XKB_KEY_Return:    return 13;
        case XKB_KEY_KP_Enter:  return 13;
        case XKB_KEY_Tab:       return 9;
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

// Kitty modifier encoding: 1-based bitmask (1=shift, 2=alt, 4=ctrl, 8=super)
static int kitty_modifiers(KeyMod mods) {
    int m = 0;
    if (mods & KeyMod::Shift) m |= 1;
    if (mods & KeyMod::Alt)   m |= 2;
    if (mods & KeyMod::Ctrl)  m |= 4;
    if (mods & KeyMod::Super) m |= 8;
    return m;
}

// Legacy CSI sequence for functional keys.
// Returns the CSI parameter number and final char, or {0,0} if not a legacy key.
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

// Encode key event for kitty keyboard protocol
static std::string encode_key_kitty(const KeyEvent &key, const ScreenBuffer &buffer) {
    if (!key.pressed) return "";

    int flags = buffer.kitty_kbd_flags();
    int mods = kitty_modifiers(key.mods);
    int mod_param = mods + 1;  // 1-based modifier parameter

    // Check if this key has a legacy CSI representation
    LegacyKey lk = legacy_csi_key(key.keysym);
    if (lk.num != 0) {
        // Functional key with legacy CSI encoding — keep the legacy form
        // Format: CSI [num] [;mod] final
        char buf[32];
        if (lk.final_char == '~') {
            // CSI num [;mod] ~
            if (mod_param > 1)
                snprintf(buf, sizeof(buf), "\033[%d;%d~", lk.num, mod_param);
            else
                snprintf(buf, sizeof(buf), "\033[%d~", lk.num);
        } else {
            // CSI [1;mod] X  (arrows: A/B/C/D, Home: H, End: F, F1-F4: P/Q/R/S)
            if (mod_param > 1)
                snprintf(buf, sizeof(buf), "\033[1;%d%c", mod_param, lk.final_char);
            else
                snprintf(buf, sizeof(buf), "\033[%c", lk.final_char);
        }
        return buf;
    }

    // For non-functional keys, determine the keycode
    int kc = kitty_keycode(key.keysym);

    if (kc == 0) {
        // Not a named special key — use the Unicode codepoint
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

    // Decide whether we need the CSI u encoding or can use a plain char
    bool need_csi = false;

    // With disambiguate (flag bit 0): use CSI u for modified keys and special keys
    if (flags & 1) {
        bool has_significant_mods = mods != 0;
        // Shift alone on a printable key doesn't need CSI u
        if (mods == 1 && kc >= 32 && kc < 127 && !key.text.empty())
            has_significant_mods = false;

        if (has_significant_mods)
            need_csi = true;

        // Special keys (Enter, Tab, Backspace, Escape) always use CSI u
        if (kc == 27 || kc == 13 || kc == 9 || kc == 127)
            need_csi = true;
    }

    // With "report all keys" (flag bit 3): everything uses CSI u
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

    // Fall through to plain text for unmodified printable keys
    if (!key.text.empty())
        return key.text;

    return "";
}

// Encode key event into the byte sequence to send to the PTY (legacy mode)
static std::string encode_key_legacy(const KeyEvent &key, const ScreenBuffer &buffer) {
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
            seq = "\033[H";
            break;
        case XKB_KEY_End:
            seq = "\033[F";
            break;
        case XKB_KEY_Insert:
            seq = "\033[2~";
            break;
        case XKB_KEY_Delete:
            seq = "\033[3~";
            break;
        case XKB_KEY_Page_Up:
            if (shift) return ""; // handled as scroll
            seq = "\033[5~";
            break;
        case XKB_KEY_Page_Down:
            if (shift) return ""; // handled as scroll
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
            if (!key.text.empty()) {
                if (ctrl && key.text.size() == 1) {
                    char c = key.text[0];
                    if (c >= 'a' && c <= 'z') {
                        char ctrl_char = c - 'a' + 1;
                        seq = std::string(1, ctrl_char);
                        break;
                    }
                    if (c >= 'A' && c <= 'Z') {
                        char ctrl_char = c - 'A' + 1;
                        seq = std::string(1, ctrl_char);
                        break;
                    }
                }
                seq = key.text;
            }
            break;
    }

    if (!seq.empty() && alt) {
        seq = "\033" + seq;
    }

    return seq;
}

// Dispatch to kitty or legacy encoding
static std::string encode_key(const KeyEvent &key, const ScreenBuffer &buffer) {
    if (buffer.kitty_kbd_active())
        return encode_key_kitty(key, buffer);
    return encode_key_legacy(key, buffer);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGCHLD, sigchld_handler);

    Config config;
    EventLoop loop;

    // Create platform (X11)
    auto platform = Platform::create();
    if (!platform) {
        fprintf(stderr, "Failed to create platform\n");
        return 1;
    }

    // Create initial window
    int win_w = 800, win_h = 600;
    if (!platform->create_window(win_w, win_h, "rivt")) {
        fprintf(stderr, "Failed to create window\n");
        return 1;
    }

    if (!platform->create_gl_context()) {
        fprintf(stderr, "Failed to create GL context\n");
        return 1;
    }

    // Initialize renderer
    Renderer renderer;
    if (!renderer.init(config)) {
        fprintf(stderr, "Failed to initialize renderer\n");
        return 1;
    }

    renderer.set_viewport(win_w, win_h);

    // Compute initial grid size
    int cols, rows;
    renderer.compute_grid(win_w, win_h, cols, rows);

    // Create screen buffer and VT parser
    ScreenBuffer screen(cols, rows, config.scrollback_lines);
    VtParser parser(screen);

    // Title change callback
    screen.on_title_change = [&](const std::string &title) {
        platform->set_title(title);
    };

    // Spawn PTY
    Pty pty;

    // Write-back callback (for DSR, DA, kitty keyboard query responses)
    screen.on_write_back = [&](const std::string &data) {
        pty.write(data);
    };
    if (!pty.spawn(cols, rows)) {
        fprintf(stderr, "Failed to spawn PTY\n");
        return 1;
    }

    bool needs_render = true;

    // Platform callbacks
    platform->on_key = [&](const KeyEvent &key) {
        if (!key.pressed) return;

        bool ctrl  = key.mods & KeyMod::Ctrl;
        bool shift = key.mods & KeyMod::Shift;

        // Internal shortcuts
        if (ctrl && shift) {
            switch (key.keysym) {
                case XKB_KEY_V:
                case XKB_KEY_v: {
                    // Paste from clipboard
                    std::string text = platform->get_clipboard(false);
                    if (!text.empty()) {
                        if (screen.bracketed_paste()) {
                            pty.write("\033[200~");
                            pty.write(text);
                            pty.write("\033[201~");
                        } else {
                            pty.write(text);
                        }
                    }
                    return;
                }
                case XKB_KEY_C:
                case XKB_KEY_c:
                    // TODO: copy selection to clipboard
                    return;
                case XKB_KEY_plus:
                case XKB_KEY_equal: {
                    // Increase font size
                    config.font_size += 1.0f;
                    renderer.font().set_size(config.font_size, platform->get_dpi_scale() * 96.0f);
                    int new_cols, new_rows;
                    renderer.compute_grid(win_w, win_h, new_cols, new_rows);
                    screen.resize(new_cols, new_rows);
                    pty.resize(new_cols, new_rows);
                    needs_render = true;
                    return;
                }
                case XKB_KEY_minus: {
                    if (config.font_size > 6.0f) {
                        config.font_size -= 1.0f;
                        renderer.font().set_size(config.font_size, platform->get_dpi_scale() * 96.0f);
                        int new_cols, new_rows;
                        renderer.compute_grid(win_w, win_h, new_cols, new_rows);
                        screen.resize(new_cols, new_rows);
                        pty.resize(new_cols, new_rows);
                        needs_render = true;
                    }
                    return;
                }
                case XKB_KEY_0: {
                    config.font_size = 12.0f;
                    renderer.font().set_size(config.font_size, platform->get_dpi_scale() * 96.0f);
                    int new_cols, new_rows;
                    renderer.compute_grid(win_w, win_h, new_cols, new_rows);
                    screen.resize(new_cols, new_rows);
                    pty.resize(new_cols, new_rows);
                    needs_render = true;
                    return;
                }
            }
        }

        // Shift+PageUp/Down for scrolling
        if (shift && key.keysym == XKB_KEY_Page_Up) {
            screen.scroll_viewport(-rows / 2);
            needs_render = true;
            return;
        }
        if (shift && key.keysym == XKB_KEY_Page_Down) {
            screen.scroll_viewport(rows / 2);
            needs_render = true;
            return;
        }

        // Forward to PTY
        std::string seq = encode_key(key, screen);
        if (!seq.empty()) {
            // Scroll to bottom on input
            if (!screen.at_bottom()) {
                screen.scroll_to_bottom();
                needs_render = true;
            }
            pty.write(seq);
        }
    };

    platform->on_mouse = [&](const MouseEvent &mouse) {
        if (mouse.button == MouseButton::ScrollUp) {
            screen.scroll_viewport(-3);
            needs_render = true;
        } else if (mouse.button == MouseButton::ScrollDown) {
            screen.scroll_viewport(3);
            needs_render = true;
        }

        // TODO: mouse reporting to PTY if mouse_mode enabled
        // TODO: selection handling
    };

    platform->on_resize = [&](int width, int height) {
        win_w = width;
        win_h = height;
        renderer.set_viewport(width, height);

        int new_cols, new_rows;
        renderer.compute_grid(width, height, new_cols, new_rows);
        if (new_cols != cols || new_rows != rows) {
            cols = new_cols;
            rows = new_rows;
            screen.resize(cols, rows);
            pty.resize(cols, rows);
        }
        needs_render = true;
    };

    platform->on_focus = [&](bool focused) {
        renderer.set_focused(focused);
        needs_render = true;
        if (screen.focus_reporting()) {
            pty.write(focused ? "\033[I" : "\033[O");
        }
    };

    platform->on_close = [&]() {
        loop.request_quit();
    };

    // Add fds to event loop
    loop.add_fd(platform->get_event_fd(), [&](uint32_t) {
        platform->process_events();
    });

    loop.add_fd(pty.fd(), [&](uint32_t events) {
        if (events & EPOLLIN) {
            char buf[65536];
            int n;
            while ((n = pty.read(buf, sizeof(buf))) > 0) {
                parser.feed(buf, n);
                needs_render = true;
            }
            if (n < 0) {
                loop.request_quit();
            }
        }
        if (events & (EPOLLHUP | EPOLLERR)) {
            loop.request_quit();
        }
    });

    // Cursor blink timer
    bool cursor_blink_on = true;
    loop.add_timer(600, [&]() {
        cursor_blink_on = !cursor_blink_on;
        needs_render = true;
    });

    // Main loop
    while (!loop.should_quit()) {
        loop.poll(needs_render ? 0 : 16);

        if (got_sigchld) {
            got_sigchld = 0;
            if (!pty.alive()) {
                loop.request_quit();
            }
        }

        if (needs_render) {
            platform->make_current();
            renderer.render(screen, config);
            platform->swap_buffers();
            needs_render = false;
        }
    }

    return 0;
}
