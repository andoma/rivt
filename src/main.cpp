#include "core/window.h"
#include "core/event_loop.h"
#include "core/config.h"

#include <cstdio>
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
    (void)argc; (void)argv;

    signal(SIGCHLD, sigchld_handler);

    Config base_config;
    EventLoop loop;
    std::vector<std::unique_ptr<Window>> windows;

    std::function<void()> create_window;
    create_window = [&]() {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init()) return;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        raw->on_new_window = create_window;
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    };

    create_window();
    if (windows.empty()) return 1;

    loop.add_timer(600, [&]() {
        for (auto &w : windows) w->toggle_cursor_blink();
    }, true);

    while (!loop.should_quit()) {
        bool any_render = false;
        for (auto &w : windows) {
            if (w->needs_render()) { any_render = true; break; }
        }
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

        for (auto &w : windows) w->render_if_needed();
    }

    return 0;
}
