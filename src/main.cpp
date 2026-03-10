#include "core/window.h"
#include "core/event_loop.h"
#include "core/config.h"
#include "core/debug.h"

#include <cstdio>
#include <cstring>
#include <signal.h>
#include <vector>
#include <memory>
#include <algorithm>

using namespace rivt;

static volatile sig_atomic_t got_sigchld = 0;

static void sigchld_handler(int) {
    got_sigchld = 1;
}

int main(int argc, char *argv[]) {
    signal(SIGCHLD, sigchld_handler);

    // Parse global flags
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_enabled() = true;
        } else {
            args.push_back(argv[i]);
        }
    }

    Config base_config;
    EventLoop loop;
    std::vector<std::unique_ptr<Window>> windows;

    // Check for tmux subcommand: rivt tmux <args...>
    bool tmux_mode = false;
    std::vector<std::string> tmux_args;
    if (!args.empty() && args[0] == "tmux") {
        tmux_mode = true;
        for (size_t i = 1; i < args.size(); i++)
            tmux_args.push_back(args[i]);
    }

    std::function<void(Pane *)> create_tmux_window;
    std::function<void()> create_window;

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

    if (tmux_mode) {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init_tmux(tmux_args)) return 1;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    } else {
        create_window();
    }
    if (windows.empty()) return 1;

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

        if (windows.empty()) { loop.request_quit(); break; }

        // render_if_needed() calls swap_buffers() which blocks until
        // vsync, naturally pacing the loop to the display refresh rate.
        for (auto &w : windows) w->render_if_needed();
    }

    return 0;
}
