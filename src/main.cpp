#include "core/window.h"
#include "remote/remote_client.h"
#include "core/event_loop.h"
#include "core/config.h"
#include "core/debug.h"

#include <clocale>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <vector>
#include <memory>
#include <algorithm>

using namespace rivt;

// Known one-time leaks in system libraries, reported by LeakSanitizer in
// Debug builds at clean exit. Baked in so no LSAN_OPTIONS env is needed
// (lsan_suppressions.txt kept for ad-hoc additions).
extern "C" const char *__lsan_default_suppressions() {
    return "leak:libfontconfig\n"
           "leak:libexpat\n"
           "leak:libEGL_nvidia.so\n"
           "leak:libnvidia-glsi.so\n";
}

static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int) {
    got_sigchld = 1;
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    signal(SIGCHLD, sigchld_handler);

    // Parse global flags
    bool remote = false;
    std::string remote_socket;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_enabled() = true;
        } else if (strcmp(argv[i], "--remote") == 0) {
            remote = true;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            remote_socket = argv[++i];
        }
    }

    Config base_config;
    EventLoop loop;
    std::vector<std::unique_ptr<Window>> windows;

    std::function<void(Pane *)> create_tmux_window;
    std::function<void()> create_window;
    std::function<void(uint32_t)> create_remote_window;

    create_tmux_window = [&](Pane *gateway) {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init_tmux_pty(gateway)) return;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    };

    create_remote_window = [&](uint32_t attach_sid) {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init_remote(remote_socket, attach_sid)) return;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        // New windows from a remote window are new daemon sessions.
        raw->on_new_window = [&create_remote_window]() { create_remote_window(0); };
        raw->on_new_tmux_window = create_tmux_window;
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    };

    create_window = [&]() {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init()) return;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        raw->on_new_window = create_window;
        raw->on_new_tmux_window = create_tmux_window;
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    };

    if (remote) {
        // One window per existing session, so a restart resumes all of
        // them; a fresh session when the daemon holds none.
        std::string path = remote_socket.empty() ? RemoteClient::default_socket_path()
                                                 : remote_socket;
        std::vector<RemoteSessionInfo> sessions;
        RemoteClient::query_sessions(path, /*autostart=*/true, sessions);
        if (sessions.empty()) {
            create_remote_window(0);
        } else {
            for (const auto &si : sessions) create_remote_window(si.id);
        }
    } else {
        create_window();
    }
    if (windows.empty()) return 1;

    // Process-wide handlers used by macOS for menu items and the dock
    // (Cmd-N from the menu, Cmd-Q routed through our cleanup, dock-icon
    // reopen). On Linux these are stored but never called.
    Platform::set_new_window_handler(create_window);
    Platform::set_quit_handler([&]() { loop.request_quit(); });

    loop.add_timer(600, [&]() {
        for (auto &w : windows) w->toggle_cursor_blink();
    }, true);

    while (!loop.should_quit()) {
        bool any_render = false;
        for (auto &w : windows) {
            if (w->needs_render()) { any_render = true; break; }
        }

        // Drain pending I/O without blocking when a render is pending.
        // Otherwise sleep until an event arrives (up to 16ms for cursor blink).
        loop.poll(any_render ? 0 : 16);

        // Clean up closing windows (deferred from on_close callback)
        std::erase_if(windows, [&](auto &w) {
            if (w->is_closing()) {
                loop.remove_fd(w->event_fd());
                return true;
            }
            return false;
        });

        if (got_sigchld) {
            got_sigchld = 0;
            for (int i = (int)windows.size() - 1; i >= 0; i--) {
                if (!windows[i]->reap_dead_panes()) {
                    loop.remove_fd(windows[i]->event_fd());
                    windows.erase(windows.begin() + i);
                }
            }
        }

#ifdef __APPLE__
        // Standard macOS behavior: keep the app running even with no
        // windows open. The user reopens via Cmd-N or the dock icon and
        // quits explicitly via Cmd-Q (handled by the app delegate).
#else
        if (windows.empty()) { loop.request_quit(); break; }
#endif

        // render_if_needed() calls swap_buffers() which blocks until
        // vsync, naturally pacing the loop to the display refresh rate.
        for (auto &w : windows) w->render_if_needed();

        // EGL shares our X connection, so eglSwapBuffers' round trips read
        // the socket and can move events (e.g. SelectionRequest) into xcb's
        // internal queue. The socket then looks idle to epoll, so drain the
        // queue here or those events wait until unrelated X traffic arrives.
        // Index-based: process_events() can append windows via Ctrl-Shift-N.
        for (size_t i = 0; i < windows.size(); i++)
            windows[i]->platform()->process_events();
    }

    return 0;
}
