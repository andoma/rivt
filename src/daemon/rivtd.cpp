// rivtd — session daemon. Owns PTYs and authoritative terminal state;
// clients attach over a unix socket using the proto framing
// (channel 0 = control, channel N = pane N's byte stream).
//
// Object lifetimes follow the same rule as the UI: nothing is destroyed
// from inside its own fd callback. Deaths are recorded and swept after
// each poll iteration.
#include "core/config.h"
#include "core/event_loop.h"
#include "core/layout.h"
#include "core/pane.h"
#include "proto/frame.h"
#include "proto/messages.h"
#include "proto/snapshot.h"
#include "proto/wire.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <errno.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rivt {

using proto::MsgType;

static constexpr size_t CLIENT_OUT_MAX = 8 << 20;  // kill client past this
static constexpr int ATTACH_SCROLLBACK_LINES = 2000;

struct Client {
    int fd = -1;
    std::string in;
    std::string out;
    size_t out_off = 0;
    uint32_t attached = 0;  // session id, 0 = not attached
    bool hello = false;
    bool dead = false;
    bool write_armed = false;
};

// One window = one tab client-side. Layout is per window, in cell units
// (1x1 cells, 1-cell dividers).
struct SrvWindow {
    uint32_t id;
    LayoutTree layout;
    std::vector<std::pair<uint16_t, std::unique_ptr<Pane>>> panes;
};

struct Session {
    uint32_t id;
    std::string name;
    int cols = 80, rows = 24;   // session grid; every window fills it
    std::vector<SrvWindow> windows;

    size_t pane_count() const {
        size_t n = 0;
        for (const auto &w : windows) n += w.panes.size();
        return n;
    }
};

struct PaneRef {
    uint32_t sid;
    uint32_t wid;
    Pane *pane;
};

class Daemon {
public:
    Daemon(std::string socket_path) : m_path(std::move(socket_path)) {}

    bool init() {
        // Signals via signalfd: SIGCHLD for reaping, SIGTERM/SIGINT to quit.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGPIPE);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        m_sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (m_sig_fd < 0) { perror("signalfd"); return false; }
        m_loop.add_fd(m_sig_fd, [this](uint32_t) { on_signal(); });

        if (!setup_socket()) return false;
        m_loop.add_fd(m_listen_fd, [this](uint32_t) { on_accept(); });
        return true;
    }

    int run() {
        while (m_loop.poll(-1)) sweep();
        return 0;
    }

private:
    bool setup_socket() {
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(addr.sun_path)) {
            fprintf(stderr, "socket path too long: %s\n", m_path.c_str());
            return false;
        }
        strcpy(addr.sun_path, m_path.c_str());

        m_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0) { perror("socket"); return false; }

        if (bind(m_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (errno != EADDRINUSE) { perror("bind"); return false; }
            // Stale socket? If nobody answers, take over.
            int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            bool live = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            close(probe);
            if (live) {
                fprintf(stderr, "rivtd already running on %s\n", m_path.c_str());
                return false;
            }
            unlink(m_path.c_str());
            if (bind(m_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                perror("bind");
                return false;
            }
        }
        chmod(m_path.c_str(), 0600);
        if (listen(m_listen_fd, 8) < 0) { perror("listen"); return false; }
        return true;
    }

    void on_signal() {
        struct signalfd_siginfo si;
        while (read(m_sig_fd, &si, sizeof si) == sizeof si) {
            if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
                m_loop.request_quit();
            if (si.ssi_signo == SIGCHLD)
                while (waitpid(-1, nullptr, WNOHANG) > 0) {}
        }
    }

    // ---------------- client I/O ----------------

    void on_accept() {
        int fd;
        while ((fd = accept4(m_listen_fd, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
            auto c = std::make_unique<Client>();
            c->fd = fd;
            Client *raw = c.get();
            m_clients.push_back(std::move(c));
            m_loop.add_fd(fd, [this, raw](uint32_t ev) { client_event(raw, ev); });
        }
    }

    void client_event(Client *c, uint32_t ev) {
        if (c->dead) return;
        if (ev & (EV_HUP | EV_ERR)) { kill_client(c); return; }
        if (ev & EV_WRITE) flush_client(c);
        if (ev & EV_READ) {
            char buf[65536];
            for (;;) {
                ssize_t n = recv(c->fd, buf, sizeof buf, 0);
                if (n > 0) { c->in.append(buf, n); continue; }
                if (n == 0) { kill_client(c); return; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                kill_client(c);
                return;
            }
            process_client(c);
        }
    }

    void process_client(Client *c) {
        while (!c->dead && c->in.size() >= proto::FRAME_HEADER_SIZE) {
            proto::FrameHeader h;
            if (!proto::decode_frame_header((const uint8_t *)c->in.data(), h)) {
                kill_client(c);
                return;
            }
            size_t total = proto::FRAME_HEADER_SIZE + h.len;
            if (c->in.size() < total) return;
            const uint8_t *payload = (const uint8_t *)c->in.data() + proto::FRAME_HEADER_SIZE;
            if (h.channel == 0)
                handle_control(c, (MsgType)h.type, payload, h.len);
            else if (h.type == proto::PANE_IN)
                handle_pane_input(c, h.channel, payload, h.len);
            c->in.erase(0, total);
        }
    }

    void send_frame(Client *c, uint16_t channel, uint16_t type,
                    const void *data, size_t len) {
        if (c->dead) return;
        if (c->out.size() - c->out_off + len > CLIENT_OUT_MAX) {
            // Slow/stuck client. Design says snapshot-resync; v1 policy
            // is disconnect (the client reconnects and re-attaches).
            kill_client(c);
            return;
        }
        uint8_t hdr[proto::FRAME_HEADER_SIZE];
        proto::encode_frame_header(hdr, {(uint32_t)len, channel, type});
        c->out.append((const char *)hdr, sizeof hdr);
        c->out.append((const char *)data, len);
        flush_client(c);
    }

    void send_control(Client *c, MsgType t, const proto::Writer &w) {
        send_frame(c, 0, (uint16_t)t, w.buf.data(), w.buf.size());
    }

    void flush_client(Client *c) {
        while (c->out_off < c->out.size()) {
            ssize_t n = send(c->fd, c->out.data() + c->out_off,
                             c->out.size() - c->out_off, MSG_NOSIGNAL);
            if (n > 0) { c->out_off += n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            kill_client(c);
            return;
        }
        if (c->out_off == c->out.size()) {
            c->out.clear();
            c->out_off = 0;
        }
        bool want_write = c->out_off < c->out.size();
        if (want_write != c->write_armed) {
            c->write_armed = want_write;
            m_loop.modify_fd(c->fd, want_write ? (EV_READ | EV_WRITE) : EV_READ);
        }
    }

    void kill_client(Client *c) {
        if (c->dead) return;
        c->dead = true;
        m_sweep_clients = true;
    }

    // ---------------- control handling ----------------

    void handle_control(Client *c, MsgType t, const uint8_t *data, size_t len) {
        proto::Reader r(data, len);
        if (!c->hello) {
            if (t != MsgType::Hello || r.u32() != proto::PROTO_VERSION || !r.ok) {
                kill_client(c);
                return;
            }
            c->hello = true;
            proto::Writer w;
            w.u32(proto::PROTO_VERSION);
            send_control(c, MsgType::HelloOk, w);
            return;
        }

        switch (t) {
        case MsgType::ListSessions: {
            proto::Writer w;
            w.u32((uint32_t)m_sessions.size());
            for (auto &[id, s] : m_sessions) {
                w.u32(id);
                w.str(s.name);
                w.u32((uint32_t)s.pane_count());
            }
            send_control(c, MsgType::SessionList, w);
            break;
        }
        case MsgType::CreateSession: {
            std::string name = r.str(), cwd = r.str();
            int cols = r.u16(), rows = r.u16();
            if (!r.ok || cols < 2 || rows < 2 || cols > 4096 || rows > 4096) {
                kill_client(c);
                return;
            }
            create_session(c, name, cwd, cols, rows);
            break;
        }
        case MsgType::Attach: {
            uint32_t sid = r.u32();
            auto it = m_sessions.find(sid);
            if (!r.ok || it == m_sessions.end()) {
                send_error(c, "no such session");
                return;
            }
            c->attached = sid;
            Session &s = it->second;
            proto::Writer w;
            w.u32(sid);
            send_control(c, MsgType::AttachOk, w);
            for (auto &win : s.windows) {
                proto::Writer wa;
                wa.u32(sid);
                wa.u32(win.id);
                send_control(c, MsgType::WindowAdded, wa);
                send_layout_to(c, s, win);
            }
            for (auto &win : s.windows)
                for (auto &[pid, pane] : win.panes) {
                    auto blob = proto::Snapshot::serialize(pane->screen(), pane->parser(),
                                                           ATTACH_SCROLLBACK_LINES);
                    send_frame(c, pid, proto::PANE_SNAPSHOT, blob.data(), blob.size());
                }
            break;
        }
        case MsgType::Detach:
            c->attached = 0;
            break;
        case MsgType::Resize: {
            int cols = r.u16(), rows = r.u16();
            auto it = m_sessions.find(c->attached);
            if (!r.ok || it == m_sessions.end()) return;
            if (cols < 2 || rows < 2 || cols > 4096 || rows > 4096) return;
            it->second.cols = cols;
            it->second.rows = rows;
            for (auto &win : it->second.windows) relayout(it->second, win);
            break;
        }
        case MsgType::Split: {
            uint32_t pid = r.u32();
            uint8_t dir = r.u8();
            auto pit = m_panes.find((uint16_t)pid);
            if (!r.ok || pit == m_panes.end() || pit->second.sid != c->attached) return;
            split_pane(m_sessions.at(c->attached), pit->second.wid, pit->second.pane,
                       dir == 0 ? SplitDir::Vertical : SplitDir::Horizontal);
            break;
        }
        case MsgType::ClosePane: {
            uint32_t pid = r.u32();
            auto pit = m_panes.find((uint16_t)pid);
            if (!r.ok || pit == m_panes.end() || pit->second.sid != c->attached) return;
            // Explicit removal: closing our master fd delivers no HUP to
            // us, so the pane-death path would never fire. We're inside a
            // client callback (not the pane's), so immediate teardown is
            // safe. Detach before close so the fd leaves the loop first.
            Pane *pane = pit->second.pane;
            pane->detach(m_loop);
            pane->pty().close();
            m_panes.erase(pit);
            remove_dead_pane((uint16_t)pid);
            break;
        }
        case MsgType::NewWindow: {
            auto it = m_sessions.find(c->attached);
            if (it == m_sessions.end()) return;
            add_window(it->second, "");
            break;
        }
        case MsgType::CloseWindow: {
            uint32_t wid = r.u32();
            auto it = m_sessions.find(c->attached);
            if (!r.ok || it == m_sessions.end()) return;
            close_window(it->second, wid);
            break;
        }
        case MsgType::KillSession: {
            uint32_t sid = r.u32();
            auto it = m_sessions.find(sid);
            if (!r.ok || it == m_sessions.end()) return;
            close_session(it->second);
            m_sessions.erase(it);
            break;
        }
        default:
            break;
        }
    }

    void handle_pane_input(Client *c, uint16_t pane_id, const uint8_t *data, size_t len) {
        if (!c->hello) { kill_client(c); return; }
        auto it = m_panes.find(pane_id);
        if (it == m_panes.end() || it->second.sid != c->attached) return;
        it->second.pane->write((const char *)data, len);
    }

    void send_error(Client *c, const char *msg) {
        proto::Writer w;
        w.str(msg);
        send_control(c, MsgType::Error, w);
    }

    // ---------------- sessions ----------------

    void create_session(Client *c, const std::string &name, const std::string &cwd,
                        int cols, int rows) {
        uint32_t sid = m_next_sid++;
        Session s;
        s.id = sid;
        s.name = name.empty() ? ("session-" + std::to_string(sid)) : name;
        s.cols = cols;
        s.rows = rows;
        m_sessions.emplace(sid, std::move(s));

        uint16_t pid = add_window(m_sessions.at(sid), cwd);
        if (!pid) {
            m_sessions.erase(sid);
            proto::Writer w;
            w.u32(0);
            w.u32(0);
            w.str("failed to spawn shell");
            send_control(c, MsgType::SessionCreated, w);
            return;
        }

        proto::Writer w;
        w.u32(sid);
        w.u32(pid);
        w.str("");
        send_control(c, MsgType::SessionCreated, w);
    }

    // Create a window with one shell pane; announces WindowAdded and the
    // window's layout to attached clients. Returns the pane id, 0 on failure.
    uint16_t add_window(Session &s, const std::string &cwd) {
        uint16_t pid = m_next_pane++;
        auto pane = std::make_unique<Pane>(s.cols, s.rows, m_config);
        if (!pane->spawn_shell(m_loop, cwd)) return 0;

        SrvWindow win;
        win.id = m_next_wid++;
        win.layout.init(pane.get());
        wire_pane(s.id, pid, pane.get());
        win.panes.emplace_back(pid, std::move(pane));
        m_panes[pid] = {s.id, win.id, win.panes.back().second.get()};
        s.windows.push_back(std::move(win));

        proto::Writer w;
        w.u32(s.id);
        w.u32(s.windows.back().id);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id)
                send_control(c.get(), MsgType::WindowAdded, w);
        relayout(s, s.windows.back());
        return pid;
    }

    void close_window(Session &s, uint32_t wid) {
        auto wit = std::find_if(s.windows.begin(), s.windows.end(),
                                [wid](const SrvWindow &w) { return w.id == wid; });
        if (wit == s.windows.end()) return;
        for (auto &[pid, pane] : wit->panes) {
            pane->detach(m_loop);
            pane->pty().close();
            m_panes.erase(pid);
        }
        proto::Writer w;
        w.u32(s.id);
        w.u32(wid);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id)
                send_control(c.get(), MsgType::WindowClosed, w);
        s.windows.erase(wit);
        if (s.windows.empty()) {
            uint32_t sid = s.id;
            close_session(s);
            m_sessions.erase(sid);
        }
    }

    void wire_pane(uint32_t sid, uint16_t pid, Pane *pane) {
        pane->on_output = [this, sid, pid](const char *d, size_t n) {
            for (auto &c : m_clients)
                if (!c->dead && c->attached == sid)
                    send_frame(c.get(), pid, proto::PANE_OUT, d, n);
        };
        pane->on_dead = [this, pid](Pane *) {
            if (m_panes.erase(pid))
                m_dead_panes.push_back(pid);
        };
        ScreenBuffer &sb = pane->screen();
        sb.on_write_back = [pane](const std::string &d) { pane->write(d); };
        sb.on_title_change = [this, sid, pid](const std::string &title) {
            broadcast_event(sid, MsgType::TitleChanged, pid, title);
        };
        sb.on_cwd_change = [this, sid, pid, pane](const std::string &uri) {
            pane->cwd = uri;
            broadcast_event(sid, MsgType::CwdChanged, pid, uri);
        };
        sb.on_bell = [this, sid, pid]() {
            broadcast_event(sid, MsgType::Bell, pid, {});
        };
        // OSC 52 clipboard: needs most-recently-active routing (design);
        // not wired in v1.
    }

    // Recompute a window's cell-unit layout (this resizes panes and
    // their PTYs to the computed rects) and broadcast it.
    void relayout(Session &s, SrvWindow &win) {
        win.layout.compute_layout(0, 0, s.cols, s.rows, /*divider=*/1,
                                  /*cell_w=*/1, /*cell_h=*/1);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id) send_layout_to(c.get(), s, win);
    }

    void send_layout_to(Client *c, Session &s, SrvWindow &win) {
        proto::Writer w;
        w.u32(s.id);
        w.u32(win.id);
        w.u16((uint16_t)s.cols);
        w.u16((uint16_t)s.rows);
        w.u32((uint32_t)win.panes.size());
        for (auto &[pid, pane] : win.panes) {
            w.u32(pid);
            w.u16((uint16_t)pane->rect.x);
            w.u16((uint16_t)pane->rect.y);
            w.u16((uint16_t)pane->rect.w);
            w.u16((uint16_t)pane->rect.h);
        }
        send_control(c, MsgType::LayoutUpdate, w);
    }

    static std::string osc7_path(const std::string &uri) {
        const std::string prefix = "file://";
        if (uri.rfind(prefix, 0) != 0) return uri;
        auto slash = uri.find('/', prefix.size());
        return slash == std::string::npos ? "" : uri.substr(slash);
    }

    void split_pane(Session &s, uint32_t wid, Pane *target, SplitDir dir) {
        auto wit = std::find_if(s.windows.begin(), s.windows.end(),
                                [wid](const SrvWindow &w) { return w.id == wid; });
        if (wit == s.windows.end()) return;
        uint16_t pid = m_next_pane++;
        auto pane = std::make_unique<Pane>(target->screen().cols(),
                                           target->screen().rows(), m_config);
        if (!pane->spawn_shell(m_loop, osc7_path(target->cwd))) return;
        if (!wit->layout.split(target, pane.get(), dir)) {
            pane->detach(m_loop);
            pane->pty().close();
            return;
        }
        wire_pane(s.id, pid, pane.get());
        wit->panes.emplace_back(pid, std::move(pane));
        m_panes[pid] = {s.id, wid, wit->panes.back().second.get()};
        relayout(s, *wit);
    }

    void broadcast_event(uint32_t sid, MsgType t, uint16_t pid, const std::string &str_arg) {
        proto::Writer w;
        w.u32(pid);
        if (t == MsgType::TitleChanged || t == MsgType::CwdChanged) w.str(str_arg);
        for (auto &c : m_clients)
            if (!c->dead && c->hello && c->attached == sid)
                send_control(c.get(), t, w);
    }

    void close_session(Session &s) {
        for (auto &win : s.windows)
            for (auto &[pid, pane] : win.panes) {
                m_panes.erase(pid);
                pane->detach(m_loop);
                pane->pty().close();  // HUPs the child
            }
        proto::Writer w;
        w.u32(s.id);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id) {
                send_control(c.get(), MsgType::SessionClosed, w);
                c->attached = 0;
            }
    }

    // ---------------- deferred cleanup ----------------

    // Remove a pane whose PTY is already dead/closed: notify clients,
    // collapse the layout, close the window when it was the window's
    // last pane, and end the session when it was the last window.
    void remove_dead_pane(uint16_t pid) {
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            Session &s = it->second;
            for (auto wit = s.windows.begin(); wit != s.windows.end(); ++wit) {
                auto pit = std::find_if(wit->panes.begin(), wit->panes.end(),
                                        [pid](auto &p) { return p.first == pid; });
                if (pit == wit->panes.end()) continue;

                proto::Writer w;
                w.u32(pid);
                for (auto &c : m_clients)
                    if (!c->dead && c->attached == s.id)
                        send_control(c.get(), MsgType::PaneExited, w);

                wit->layout.remove(pit->second.get());
                pit->second->detach(m_loop);
                wit->panes.erase(pit);
                if (!wit->panes.empty()) {
                    relayout(s, *wit);
                    return;
                }
                close_window(s, wit->id);
                return;
            }
        }
    }

    void sweep() {
        for (uint16_t pid : m_dead_panes) remove_dead_pane(pid);
        m_dead_panes.clear();

        if (m_sweep_clients) {
            m_sweep_clients = false;
            for (auto &c : m_clients)
                if (c->dead) {
                    m_loop.remove_fd(c->fd);
                    close(c->fd);
                }
            std::erase_if(m_clients, [](auto &c) { return c->dead; });
        }
    }

    std::string m_path;
    Config m_config;
    EventLoop m_loop;
    int m_listen_fd = -1;
    int m_sig_fd = -1;
    std::vector<std::unique_ptr<Client>> m_clients;
    std::map<uint32_t, Session> m_sessions;
    std::unordered_map<uint16_t, PaneRef> m_panes;
    uint32_t m_next_sid = 1;
    uint32_t m_next_wid = 1;
    uint16_t m_next_pane = 1;
    std::vector<uint16_t> m_dead_panes;
    bool m_sweep_clients = false;
};

} // namespace rivt

static std::string default_socket_path() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    std::string dir = rt ? std::string(rt) + "/rivt"
                         : "/tmp/rivt-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700);
    return dir + "/daemon.sock";
}

int main(int argc, char **argv) {
    std::string path;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) path = argv[++i];
    }
    if (path.empty()) path = default_socket_path();

    rivt::Daemon d(path);
    if (!d.init()) return 1;
    fprintf(stderr, "rivtd: listening on %s\n", path.c_str());
    return d.run();
}
