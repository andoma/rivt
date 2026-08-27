#include "remote/remote_client.h"
#include "core/debug.h"
#include "proto/frame.h"
#include "proto/messages.h"
#include "net/identity.h"
#include "net/sock.h"

#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rivt {

using proto::MsgType;

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void RemoteClient::note_rx() { m_last_rx_ms = now_ms(); }
double RemoteClient::seconds_since_rx() const {
    return m_last_rx_ms ? (now_ms() - m_last_rx_ms) / 1000.0 : 0.0;
}
void RemoteClient::set_link(const std::string &st) {
    if (m_link_state == st) return;
    m_link_state = st;
    if (on_status) on_status();
}

std::string RemoteClient::default_socket_path() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    std::string dir = rt ? std::string(rt) + "/rivt"
                         : "/tmp/rivt-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700);
    return dir + "/daemon.sock";
}

static int try_connect(const std::string &path) {
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return -1;
    strcpy(addr.sun_path, path.c_str());
    int fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Spawn rivtd detached (double fork so it isn't our child to reap).
// Looks for rivtd next to our own binary first, then falls back to PATH.
static void spawn_daemon(const std::string &socket_path) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild != 0) _exit(0);
        setsid();

        // Detach stdio: the daemon must not write into whichever
        // terminal happened to spawn it. Its stderr goes to a log next
        // to the socket.
        std::string log = socket_path + ".log";
        int devnull = open("/dev/null", O_RDWR);
        int logfd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); }
        if (logfd >= 0) dup2(logfd, 2);
        else if (devnull >= 0) dup2(devnull, 2);

        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            std::string sibling = std::string(dirname(self)) + "/rivtd";
            execl(sibling.c_str(), "rivtd", "--socket", socket_path.c_str(), (char *)nullptr);
        }
        execlp("rivtd", "rivtd", "--socket", socket_path.c_str(), (char *)nullptr);
        _exit(127);
    }
    waitpid(pid, nullptr, 0);  // reap the intermediate child
}

enum class QueryResult { Ok, NoDaemon, Mismatch };

// One blocking hello+list roundtrip. Sets mismatch when the daemon
// rejects our protocol version.
static QueryResult query_once(const std::string &path, std::vector<RemoteSessionInfo> &out) {
    int fd = try_connect(path);
    if (fd < 0) return QueryResult::NoDaemon;

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    auto send_msg = [fd](uint16_t type, const proto::Writer &w) {
        uint8_t hdr[proto::FRAME_HEADER_SIZE];
        proto::encode_frame_header(hdr, {(uint32_t)w.buf.size(), 0, type});
        (void)!write(fd, hdr, sizeof hdr);
        if (!w.buf.empty()) (void)!write(fd, w.buf.data(), w.buf.size());
    };
    proto::Writer hello;
    hello.u32(proto::PROTO_VERSION);
    send_msg((uint16_t)MsgType::Hello, hello);
    send_msg((uint16_t)MsgType::ListSessions, {});

    std::string buf;
    QueryResult res = QueryResult::NoDaemon;
    bool done = false;
    while (!done) {
        char tmp[4096];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) break;
        buf.append(tmp, n);
        while (!done && buf.size() >= proto::FRAME_HEADER_SIZE) {
            proto::FrameHeader h;
            if (!proto::decode_frame_header((const uint8_t *)buf.data(), h)) { done = true; break; }
            if (buf.size() < proto::FRAME_HEADER_SIZE + h.len) break;
            proto::Reader r((const uint8_t *)buf.data() + proto::FRAME_HEADER_SIZE, h.len);
            if (h.channel == 0 && h.type == (uint16_t)MsgType::Error) {
                std::string e = r.str();
                if (e.rfind("protocol version mismatch", 0) == 0)
                    res = QueryResult::Mismatch;
                else
                    fprintf(stderr, "rivt: rivtd: %s\n", e.c_str());
                done = true;
            } else if (h.channel == 0 && h.type == (uint16_t)MsgType::SessionList) {
                uint32_t cnt = r.u32();
                for (uint32_t i = 0; i < cnt && r.ok; i++) {
                    RemoteSessionInfo si;
                    si.id = r.u32();
                    si.name = r.str();
                    si.npanes = r.u32();
                    out.push_back(std::move(si));
                }
                res = r.ok ? QueryResult::Ok : QueryResult::NoDaemon;
                done = true;
            }
            buf.erase(0, proto::FRAME_HEADER_SIZE + h.len);
        }
    }
    ::close(fd);
    return res;
}

// Fire-and-forget UpgradeDaemon (accepted pre-Hello, version-agnostic).
static void request_daemon_upgrade(const std::string &path) {
    int fd = try_connect(path);
    if (fd < 0) return;
    uint8_t hdr[proto::FRAME_HEADER_SIZE];
    proto::encode_frame_header(hdr, {0, 0, (uint16_t)MsgType::UpgradeDaemon});
    (void)!write(fd, hdr, sizeof hdr);
    ::close(fd);
}

bool RemoteClient::query_sessions(const std::string &path, bool autostart,
                                  std::vector<RemoteSessionInfo> &out) {
    QueryResult q = query_once(path, out);

    if (q == QueryResult::NoDaemon && autostart) {
        dbg("remote: no daemon at %s, spawning rivtd", path.c_str());
        spawn_daemon(path);
        for (int i = 0; i < 40 && q == QueryResult::NoDaemon; i++) {
            usleep(50000);
            out.clear();
            q = query_once(path, out);
        }
    }

    if (q == QueryResult::Mismatch) {
        // An older daemon is running. Ask it to upgrade itself in place
        // (sessions survive the exec) and retry. Daemons older than the
        // upgrade mechanism ignore this; they must be killed by hand.
        fprintf(stderr, "rivt: rivtd runs an older protocol, upgrading it in place...\n");
        request_daemon_upgrade(path);
        for (int i = 0; i < 40; i++) {
            usleep(100000);
            out.clear();
            q = query_once(path, out);
            if (q == QueryResult::Ok) return true;
        }
        fprintf(stderr,
                "rivt: daemon did not upgrade (too old?) — kill it by pid: pgrep -a rivtd\n");
        return false;
    }
    return q == QueryResult::Ok;
}

bool RemoteClient::connect(const RemoteEndpoint &ep, bool autostart) {
    if (!ep.is_quic()) return connect(ep.unix_path, autostart);
    m_endpoint = ep;
    if (m_quic) close();
    m_stale_quic.reset();  // fresh stack: safe to dispose parked engines
    m_stale_probes.clear();
    m_probes.clear();
    m_probe_kinds.clear();
    m_pending_out.clear();

    if (!m_identity) {
        m_identity = net::Identity::load_or_create();
        if (!m_identity) {
            fprintf(stderr, "rivt: cannot create device identity\n");
            return false;
        }
    }
    std::string bundle = net::Identity::authorized_bundle_path();
    fprintf(stderr, "rivt: connecting, racing %zu candidate(s):\n",
            ep.candidates.size());
    for (const auto &c : ep.candidates) {
        fprintf(stderr, "rivt:   [%-8s] %s:%u\n", c.kind.c_str(), c.host.c_str(), c.port);
        auto probe = net::QuicEngine::connect(m_loop, c.host, c.port, *m_identity, bundle);
        if (!probe) continue;
        size_t idx = m_probes.size();
        probe->on_connected = [this, idx](net::QuicEngine::Conn *) { adopt_probe(idx); };
        probe->on_closed = [this](net::QuicEngine::Conn *) { probe_failed(); };
        m_probes.push_back(std::move(probe));
        m_probe_kinds.push_back(c.kind);
    }
    begin_punch(ep);
    return !m_probes.empty() || m_signaling != nullptr;
}

// Open the signaling channel and start a NAT hole punch: STUN two
// sockets (one for a direct punched path, one for the TURN relay),
// offer both reflexive addresses, and on the server's answer connect
// each to the matching server candidate. Runs alongside the direct
// LAN probes; whichever validates first wins.
void RemoteClient::begin_punch(const RemoteEndpoint &ep) {
    if (ep.peer_sig_id.empty() || ep.rendezvous.empty()) {
        fprintf(stderr, "rivt: punch skipped (sig_id=%zu rdv=%zu)\n",
                ep.peer_sig_id.size(), ep.rendezvous.size());
        return;
    }
    fprintf(stderr, "rivt: punch: opening signaling to %.16s...\n", ep.peer_sig_id.c_str());
    m_signaling = std::make_unique<net::Signaling>(m_loop, *m_identity);
    m_signaling->on_candidates =
        [this](const std::string &, bool answer, std::vector<net::Candidate> c) {
            if (answer) on_answer(c);
        };
    if (!m_signaling->start(ep.rendezvous)) { m_signaling.reset(); return; }

    std::string bundle = net::Identity::authorized_bundle_path();
    std::string peer = ep.peer_sig_id;
    // Two engines: [0]=direct, [1]=turn. Add as probes so on_connected
    // adopts them like any candidate.
    auto reflexives = std::make_shared<std::vector<net::Candidate>>();
    auto pending = std::make_shared<int>(2);
    for (int role = 0; role < 2; role++) {
        auto e = net::QuicEngine::create_client(m_loop, *m_identity, bundle);
        if (!e) { (*pending)--; continue; }
        size_t idx = m_probes.size();
        e->on_connected = [this, idx](net::QuicEngine::Conn *) { adopt_probe(idx); };
        e->on_closed = [this](net::QuicEngine::Conn *) { probe_failed(); };
        net::QuicEngine *ep_raw = e.get();
        m_probes.push_back(std::move(e));
        m_probe_kinds.push_back(role == 0 ? "direct" : "turn");
        ep_raw->discover_reflexive([this, ep_raw, reflexives, pending, peer]
                                   (bool ok, const struct sockaddr_storage &sa) {
            (void)ep_raw;
            if (ok) {
                char ip[64] = {0};
                uint16_t port = 0;
                if (sa.ss_family == AF_INET) {
                    auto *s = (const struct sockaddr_in *)&sa;
                    inet_ntop(AF_INET, &s->sin_addr, ip, sizeof ip); port = ntohs(s->sin_port);
                } else {
                    auto *s = (const struct sockaddr_in6 *)&sa;
                    inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof ip); port = ntohs(s->sin6_port);
                }
                reflexives->push_back({ip, port, "stun"});
            }
            if (--(*pending) == 0 && m_signaling && !reflexives->empty()) {
                fprintf(stderr, "rivt: punch: sending offer, %zu reflexive candidate(s)\n",
                        reflexives->size());
                m_signaling->send(peer, /*answer=*/false, *reflexives);
            }
        });
    }
}

void RemoteClient::on_answer(const std::vector<net::Candidate> &server_cands) {
    if (m_quic) return;  // already connected via a direct LAN probe
    // m_probes[.. last two] are our [direct, turn] engines (created in
    // begin_punch after the LAN probes).
    net::QuicEngine *direct = nullptr, *turn = nullptr;
    size_t n = m_probes.size();
    if (n >= 2) { direct = m_probes[n - 2].get(); turn = m_probes[n - 1].get(); }
    const net::Candidate *cstun = nullptr, *cturn = nullptr;
    std::vector<net::Candidate> locals;
    for (const auto &c : server_cands) {
        if (c.kind == "stun" && !cstun) cstun = &c;
        else if (c.kind == "turn" && !cturn) cturn = &c;
        else if (c.kind == "local") locals.push_back(c);
    }
    fprintf(stderr, "rivt: peer answered (%s%s%zu local); punching\n",
            cstun ? "stun " : "", cturn ? "turn " : "", locals.size());
    if (direct && cstun) direct->start_connection(cstun->host, cstun->port);
    if (turn && cturn) turn->start_connection(cturn->host, cturn->port);
    // Server-side LAN addresses, in case any is reachable from us.
    std::string bundle = net::Identity::authorized_bundle_path();
    for (const auto &c : locals) {
        auto e = net::QuicEngine::connect(m_loop, c.host, c.port, *m_identity, bundle);
        if (!e) continue;
        size_t idx = m_probes.size();
        e->on_connected = [this, idx](net::QuicEngine::Conn *) { adopt_probe(idx); };
        e->on_closed = [this](net::QuicEngine::Conn *) { probe_failed(); };
        m_probes.push_back(std::move(e));
        m_probe_kinds.push_back("local");
    }
}

void RemoteClient::adopt_probe(size_t idx) {
    if (m_quic) return;  // someone else won already
    if (idx < m_probe_kinds.size()) {
        const std::string &k = m_probe_kinds[idx];
        m_transport = (k == "turn") ? "relay" : (k == "local") ? "lan" : "direct";
    }
    note_rx();
    set_link("connected");
    m_quic = std::move(m_probes[idx]);
    m_quic_conn = m_quic->client_conn();
    // Losing probes are parked, never destroyed here — engines must not
    // die on any probe's callback stack.
    for (auto &p : m_probes)
        if (p) m_stale_probes.push_back(std::move(p));
    m_probes.clear();

    m_quic->on_connected = nullptr;
    m_quic->on_data = [this](net::QuicEngine::Conn *, const uint8_t *d, size_t n) {
        note_rx();
        m_in.append((const char *)d, n);
        process();
    };
    m_quic->on_closed = [this](net::QuicEngine::Conn *) {
        // fail() must see connected()==true to run; close() clears the conn.
        fail();
    };
    if (!m_pending_out.empty()) {
        m_quic->send(m_quic_conn, m_pending_out.data(), m_pending_out.size());
        m_pending_out.clear();
    }
}

void RemoteClient::probe_failed() {
    if (m_quic) return;  // race lost after adoption: irrelevant
    bool cert = false;
    for (auto &p : m_probes) {
        if (p && p->client_conn() && !p->client_conn()->dead) return;  // others still trying
        if (p && p->cert_rejected()) cert = true;
    }
    // All candidates failed. Park (we're on one of their stacks).
    for (auto &p : m_probes)
        if (p) m_stale_probes.push_back(std::move(p));
    m_probes.clear();
    m_pending_out.clear();
    if (cert) {
        fprintf(stderr,
                "rivt: reached the peer, but its certificate was rejected.\n"
                "      The two devices are not in the same set. Pair them:\n"
                "      on one run `rivt pair` / `rivtd pair`, on the other "
                "`rivt setup <code>` / `rivtd setup <code>`.\n");
    } else {
        fprintf(stderr,
                "rivt: no candidate address responded — the peer's QUIC port "
                "(udp/%u) is not reachable from here.\n"
                "      Likely NAT/firewall between the two networks, or rivtd "
                "not listening. RIVT_QUIC_DEBUG=1 shows per-packet traffic.\n",
                m_endpoint.candidates.empty() ? 0 : m_endpoint.candidates.front().port);
    }
    if (on_disconnect) on_disconnect();
}

bool RemoteClient::connect(const std::string &path, bool autostart) {
    m_endpoint = RemoteEndpoint{path, {}};
    m_fd = try_connect(path);
    if (m_fd < 0 && autostart) {
        dbg("remote: no daemon at %s, spawning rivtd", path.c_str());
        spawn_daemon(path);
        for (int i = 0; i < 40 && m_fd < 0; i++) {
            usleep(50000);
            m_fd = try_connect(path);
        }
    }
    if (m_fd < 0) return false;

    fcntl(m_fd, F_SETFL, O_NONBLOCK);
    m_loop.add_fd(m_fd, [this](uint32_t ev) { on_event(ev); });
    return true;
}

void RemoteClient::close() {
    if (m_quic) {
        m_quic_conn = nullptr;
        // Never destroy the engine here — we may be on its own callback
        // stack. Park it; disposed at the next connect or in ~RemoteClient.
        m_stale_quic = std::move(m_quic);
    }
    if (m_fd >= 0) {
        m_loop.remove_fd(m_fd);
        ::close(m_fd);
        m_fd = -1;
    }
    m_in.clear();
    m_out.clear();
    m_out_off = 0;
    m_write_armed = false;
    m_signaling.reset();
}

void RemoteClient::fail() {
    if (m_failing || !connected()) return;
    m_failing = true;
    close();
    if (on_disconnect) on_disconnect();
    m_failing = false;
}

void RemoteClient::on_event(uint32_t ev) {
    if (ev & EV_WRITE) flush();
    // Drain and process before honoring HUP/EOF so the daemon's final
    // frames aren't discarded when it exits.
    bool eof = false;
    if (ev & (EV_READ | EV_HUP)) {
        char buf[65536];
        for (;;) {
            ssize_t n = recv(m_fd, buf, sizeof buf, 0);
            if (n > 0) { m_in.append(buf, n); continue; }
            if (n == 0) { eof = true; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            eof = true;
            break;
        }
        process();
    }
    if (eof || (ev & (EV_HUP | EV_ERR))) fail();
}

void RemoteClient::process() {
    while (connected() && m_in.size() >= proto::FRAME_HEADER_SIZE) {
        proto::FrameHeader h;
        if (!proto::decode_frame_header((const uint8_t *)m_in.data(), h)) {
            fail();
            return;
        }
        size_t total = proto::FRAME_HEADER_SIZE + h.len;
        if (m_in.size() < total) return;
        const uint8_t *payload = (const uint8_t *)m_in.data() + proto::FRAME_HEADER_SIZE;

        if (h.channel == 0) {
            dispatch_control(h.type, payload, h.len);
        } else if (h.type == proto::PANE_OUT) {
            if (on_output) on_output(h.channel, (const char *)payload, h.len);
        } else if (h.type == proto::PANE_SNAPSHOT) {
            if (on_snapshot) on_snapshot(h.channel, payload, h.len);
        } else if (h.type == proto::PANE_SCROLLBACK) {
            if (on_scrollback) on_scrollback(h.channel, payload, h.len);
        }
        m_in.erase(0, total);
    }
}

void RemoteClient::dispatch_control(uint16_t type, const uint8_t *data, size_t len) {
    proto::Reader r(data, len);
    switch ((MsgType)type) {
    case MsgType::HelloOk: {
        uint32_t ver = r.u32();
        if (r.ok && ver == proto::PROTO_VERSION) {
            if (on_hello_ok) on_hello_ok();
        } else {
            fprintf(stderr,
                    "rivt: rivtd protocol version mismatch (daemon %u, client %u) — "
                    "restart the daemon: pkill -x rivtd\n",
                    ver, proto::PROTO_VERSION);
            fail();
        }
        break;
    }
    case MsgType::SessionList: {
        uint32_t n = r.u32();
        std::vector<RemoteSessionInfo> list;
        for (uint32_t i = 0; i < n && r.ok; i++) {
            RemoteSessionInfo s;
            s.id = r.u32();
            s.name = r.str();
            s.npanes = r.u32();
            list.push_back(std::move(s));
        }
        if (r.ok && on_session_list) on_session_list(list);
        break;
    }
    case MsgType::SessionCreated: {
        uint32_t sid = r.u32(), pane = r.u32();
        std::string err = r.str();
        if (r.ok && on_session_created) on_session_created(sid, pane, err);
        break;
    }
    case MsgType::AttachOk: {
        uint32_t sid = r.u32();
        if (r.ok && on_attach_ok) on_attach_ok(sid);
        break;
    }
    case MsgType::WindowAdded: {
        uint32_t sid = r.u32(), wid = r.u32();
        if (r.ok && on_window_added) on_window_added(sid, wid);
        break;
    }
    case MsgType::WindowClosed: {
        uint32_t sid = r.u32(), wid = r.u32();
        if (r.ok && on_window_closed) on_window_closed(sid, wid);
        break;
    }
    case MsgType::LayoutUpdate: {
        uint32_t sid = r.u32(), wid = r.u32();
        int cols = r.u16(), rows = r.u16();
        uint32_t n = r.u32();
        std::vector<RemotePaneGeom> panes;
        for (uint32_t i = 0; i < n && r.ok; i++) {
            RemotePaneGeom g;
            g.id = r.u32();
            g.x = r.u16();
            g.y = r.u16();
            g.cols = r.u16();
            g.rows = r.u16();
            panes.push_back(g);
        }
        if (r.ok && on_layout) on_layout(sid, wid, cols, rows, panes);
        break;
    }
    case MsgType::SessionClosed:
        if (on_session_closed) on_session_closed(r.u32());
        break;
    case MsgType::PaneExited:
        if (on_pane_exited) on_pane_exited(r.u32());
        break;
    case MsgType::Error:
        if (on_error) on_error(r.str());
        break;
    case MsgType::TitleChanged:
    case MsgType::CwdChanged:
    case MsgType::Bell:
        // The replica's own parser regenerates these locally; the
        // control events exist for pickers/detached observers.
        break;
    default:
        break;
    }
}

void RemoteClient::send_frame(uint16_t channel, uint16_t type, const void *data, size_t len) {
    uint8_t hdr[proto::FRAME_HEADER_SIZE];
    proto::encode_frame_header(hdr, {(uint32_t)len, channel, type});
    if (m_quic_conn) {
        m_quic->send(m_quic_conn, hdr, sizeof hdr);
        if (len) m_quic->send(m_quic_conn, data, len);
        return;
    }
    if (!m_probes.empty()) {
        // Handshake race still running: buffer until a probe wins.
        m_pending_out.append((const char *)hdr, sizeof hdr);
        m_pending_out.append((const char *)data, len);
        return;
    }
    if (m_fd < 0) return;
    m_out.append((const char *)hdr, sizeof hdr);
    m_out.append((const char *)data, len);
    flush();
}

void RemoteClient::flush() {
    while (m_fd >= 0 && m_out_off < m_out.size()) {
        ssize_t n = send(m_fd, m_out.data() + m_out_off, m_out.size() - m_out_off, MSG_NOSIGNAL);
        if (n > 0) { m_out_off += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        fail();
        return;
    }
    if (m_fd < 0) return;
    if (m_out_off == m_out.size()) {
        m_out.clear();
        m_out_off = 0;
    }
    bool want = m_out_off < m_out.size();
    if (want != m_write_armed) {
        m_write_armed = want;
        m_loop.modify_fd(m_fd, want ? (EV_READ | EV_WRITE) : EV_READ);
    }
}

void RemoteClient::hello() {
    proto::Writer w;
    w.u32(proto::PROTO_VERSION);
    send_control((uint16_t)MsgType::Hello, w);
}

void RemoteClient::list_sessions() {
    send_control((uint16_t)MsgType::ListSessions, {});
}

void RemoteClient::create_session(const std::string &name, const std::string &cwd,
                                  int cols, int rows) {
    proto::Writer w;
    w.str(name);
    w.str(cwd);
    w.u16((uint16_t)cols);
    w.u16((uint16_t)rows);
    send_control((uint16_t)MsgType::CreateSession, w);
}

void RemoteClient::attach(uint32_t sid) {
    proto::Writer w;
    w.u32(sid);
    send_control((uint16_t)MsgType::Attach, w);
}

void RemoteClient::detach() {
    send_control((uint16_t)MsgType::Detach, {});
}

void RemoteClient::resize_session(int cols, int rows) {
    proto::Writer w;
    w.u16((uint16_t)cols);
    w.u16((uint16_t)rows);
    send_control((uint16_t)MsgType::Resize, w);
}

void RemoteClient::split(uint32_t pane_id, bool horizontal) {
    proto::Writer w;
    w.u32(pane_id);
    w.u8(horizontal ? 1 : 0);
    send_control((uint16_t)MsgType::Split, w);
}

void RemoteClient::close_pane(uint32_t pane_id) {
    proto::Writer w;
    w.u32(pane_id);
    send_control((uint16_t)MsgType::ClosePane, w);
}

void RemoteClient::new_window() {
    send_control((uint16_t)MsgType::NewWindow, {});
}

void RemoteClient::fetch_scrollback(uint32_t pane_id, uint32_t end_abs, uint32_t count) {
    proto::Writer w;
    w.u32(pane_id);
    w.u32(end_abs);
    w.u32(count);
    send_control((uint16_t)MsgType::FetchScrollback, w);
}

void RemoteClient::close_window(uint32_t window_id) {
    proto::Writer w;
    w.u32(window_id);
    send_control((uint16_t)MsgType::CloseWindow, w);
}

void RemoteClient::kill_session(uint32_t sid) {
    proto::Writer w;
    w.u32(sid);
    send_control((uint16_t)MsgType::KillSession, w);
}

void RemoteClient::send_input(uint32_t pane_id, const char *data, size_t len) {
    send_frame((uint16_t)pane_id, proto::PANE_IN, data, len);
}

} // namespace rivt
