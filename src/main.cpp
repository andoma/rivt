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

// Encode key event into the byte sequence to send to the PTY
static std::string encode_key(const KeyEvent &key, const ScreenBuffer &buffer) {
    if (!key.pressed) return "";

    bool ctrl  = key.mods & KeyMod::Ctrl;
    bool shift = key.mods & KeyMod::Shift;
    bool alt   = key.mods & KeyMod::Alt;

    // Terminal internal shortcuts (not forwarded to PTY)
    // These are handled by the caller

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
