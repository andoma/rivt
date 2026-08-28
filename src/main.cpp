#include "core/window.h"
#include "remote/remote_client.h"
#include "net/rendezvous.h"
#include "net/identity.h"
#include "net/pairing.h"
#include "net/membership.h"
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
static volatile sig_atomic_t got_term = 0;

static void sigchld_handler(int) {
    got_sigchld = 1;
}

// SIGTERM/SIGINT are a clean shutdown (logout, kill, ^C): windows close
// properly, so throwaway daemon sessions are killed. SIGKILL/crash sends
// nothing and sessions survive — that's the recovery feature.
static void sigterm_handler(int) {
    got_term = 1;
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    // Membership verbs (foreground, no window). `rivt setup` is the
    // client enrollment: join a set (or found one) — no daemon/systemd.
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "setup")) {
            std::string code;
            for (int j = 1; j < argc; j++)
                if (argv[j][0] != '-' && strcmp(argv[j], "setup") != 0) { code = argv[j]; break; }
            printf("rivt client setup\n\n");
            if (!rivt::net::interactive_enroll(code)) return 1;
            printf("\nDone. Connect to a device with:  rivt --connect <name>\n");
            return 0;
        }
        if (!strcmp(argv[i], "pair")) {
            auto id = rivt::net::Identity::load_or_create();
            return id && rivt::net::pair_invite(*id) ? 0 : 1;
        }
        if (!strcmp(argv[i], "join")) {
            auto id = rivt::net::Identity::load_or_create();
            if (!id || i + 1 >= argc) { fprintf(stderr, "usage: rivt join <code>\n"); return 1; }
            return rivt::net::pair_join(argv[i + 1], *id) ? 0 : 1;
        }
    }
    signal(SIGCHLD, sigchld_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    // Parse global flags
    bool remote = false;      // daemon-backed sessions + resume all (opt-in)
    bool pick = false;        // open straight to the device picker
    std::string remote_socket;
    std::string connect_host; // QUIC daemon on another machine
    uint16_t connect_port = 7433;
    std::string peer_sig_id;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0) {
            debug_enabled() = true;
        } else if (strcmp(argv[i], "--remote") == 0) {
            remote = true;
        } else if (strcmp(argv[i], "--pick") == 0) {
            pick = true;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            remote_socket = argv[++i];
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            connect_host = argv[++i];
            auto colon = connect_host.rfind(':');
            if (colon != std::string::npos) {
                connect_port = (uint16_t)atoi(connect_host.c_str() + colon + 1);
                connect_host.resize(colon);
            }
        }
    }

    Config base_config;
    EventLoop loop;
    std::vector<std::unique_ptr<Window>> windows;

    std::function<void(Pane *)> create_tmux_window;
    std::function<void()> create_window;
    // (attach_sid, persistent): sid 0 creates a session; non-persistent
    // sessions are killed on clean window close.
    std::function<bool(uint32_t, bool)> create_remote_window;
    std::function<void()> create_picker_window;      // Ctrl-Shift-N -> device picker
    std::function<bool(const std::string &)> open_remote;  // connect to a named box
    std::vector<std::string> pending_connect;  // picker selections, opened at loop top

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

    create_remote_window = [&](uint32_t attach_sid, bool persistent) {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init_remote(remote_socket, attach_sid, !persistent)) return false;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        // New windows from a daemon-backed window get the same lifecycle.
        raw->on_new_window = create_picker_window;
        raw->on_new_tmux_window = create_tmux_window;
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
        return true;
    };

    create_window = [&]() {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init()) return;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) {
            raw->platform()->process_events();
        });
        raw->on_new_window = create_picker_window;
        raw->on_new_tmux_window = create_tmux_window;
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    };

    // Connect to a device by name (directory lookup) or host[:port].
    // Returns false if it couldn't be set up.
    open_remote = [&](const std::string &target) -> bool {
        std::vector<rivt::net::Candidate> candidates;
        std::string sig_id;
        if (target.find('.') == std::string::npos && target.find(':') == std::string::npos) {
            std::string url = rivt::net::rendezvous_url();
            if (url.empty()) {
                fprintf(stderr, "rivt: '%s' is a device name but no rendezvous is "
                                "configured\n", target.c_str());
                return false;
            }
            if (auto id = rivt::net::Identity::load_or_create())
                rivt::net::sync_membership(*id, /*found_if_missing=*/false);
            rivt::net::DirEntry e;
            if (!rivt::net::lookup_device(url, target, e)) {
                fprintf(stderr, "rivt: device '%s' not found (is rivtd --listen "
                                "running there?)\n", target.c_str());
                return false;
            }
            candidates = e.candidates;
            sig_id = e.sig_id;
            fprintf(stderr, "rivt: %s -> %zu candidate(s) from directory\n",
                    target.c_str(), candidates.size());
        } else {
            std::string host = target;
            uint16_t port = 7433;
            auto c = target.rfind(':');
            if (c != std::string::npos) { port = (uint16_t)atoi(target.c_str() + c + 1); host = target.substr(0, c); }
            candidates.push_back({host, port, "direct"});
        }
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init_remote_quic(target, candidates, sig_id, rivt::net::rendezvous_url()))
            return false;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) { raw->platform()->process_events(); });
        raw->on_new_window = create_picker_window;
        raw->on_pick_remote = [&](const std::string &n) { pending_connect.push_back(n); };
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
        return true;
    };

    create_picker_window = [&]() {
        auto win = std::make_unique<Window>(base_config, loop);
        if (!win->init_picker(rivt::net::rendezvous_url())) return;
        Window *raw = win.get();
        loop.add_fd(raw->event_fd(), [raw](uint32_t) { raw->platform()->process_events(); });
        raw->on_new_window = create_picker_window;
        raw->on_pick_remote = [&](const std::string &n) { pending_connect.push_back(n); };
        raw->on_close = [](Window *w) { w->mark_closing(); };
        windows.push_back(std::move(win));
    };

    if (!connect_host.empty()) {
        if (!open_remote(connect_host)) return 1;
    } else if (pick) {
        create_picker_window();
        if (windows.empty()) return 1;
    } else if (remote) {
        // One window per existing session, so a restart resumes all of
        // them; a fresh session when the daemon holds none.
        std::string path = remote_socket.empty() ? RemoteClient::default_socket_path()
                                                 : remote_socket;
        std::vector<RemoteSessionInfo> sessions;
        RemoteClient::query_sessions(path, /*autostart=*/true, sessions);
        if (sessions.empty()) {
            create_remote_window(0, true);
        } else {
            for (const auto &si : sessions) create_remote_window(si.id, true);
        }
    } else {
        // Plain rivt is a classic in-process terminal: no daemon, no
        // persistence. Daemons belong on remote machines (--connect) or
        // behind an explicit --remote.
        create_window();
    }
    if (windows.empty()) return 1;

    // Process-wide handlers used by macOS for menu items and the dock
    // (Cmd-N from the menu, Cmd-Q routed through our cleanup, dock-icon
    // reopen). On Linux these are stored but never called.
    Platform::set_new_window_handler(create_picker_window);
    Platform::set_quit_handler([&]() { loop.request_quit(); });
    Platform::set_connectivity_handler([&](bool awake) {
        for (auto &w : windows) w->set_awake(awake);
    });

    loop.add_timer(600, [&]() {
        for (auto &w : windows) w->toggle_cursor_blink();
    }, true);

    while (!loop.should_quit()) {
        // Picker selections are opened here, at the top of the loop, rather
        // than inline in the picker's key callback: open_remote does blocking
        // HTTPS (membership sync + directory lookup) and starts a timing-
        // sensitive hole-punch, both of which misbehave when run reentrantly
        // from inside an fd callback. This is the same context --connect uses.
        if (!pending_connect.empty()) {
            std::vector<std::string> todo;
            todo.swap(pending_connect);
            for (const auto &name : todo) open_remote(name);
        }

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

        if (got_term) {
            got_term = 0;
            for (auto &w : windows) w->mark_closing();
        }

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
        // A picker that just queued a connect closes itself in this same
        // iteration; don't quit while its replacement window is pending.
        if (windows.empty() && pending_connect.empty()) { loop.request_quit(); break; }
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
