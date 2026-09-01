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

double RemoteClient::seconds_since_rx() const {
    return m_quic ? m_quic->seconds_since_rx() : 0.0;
}
int RemoteClient::rtt_ms() const {
    return m_quic && m_quic_conn ? (int)m_quic->rtt_ms(m_quic_conn) : 0;
}
void RemoteClient::set_link(const std::string &st) {
    if (m_link_state == st) return;
    m_link_state = st;
    if (on_status) on_status();
}

std::string RemoteClient::default_socket_path() {
    // A system-unit install (rivtd install --system) binds under its
    // RuntimeDirectory instead of the caller's session env; prefer that
    // socket when a daemon actually lives there, so --upgrade and local
    // clients find it regardless of how this shell was spawned.
    {
        std::string sys = "/run/rivt-" + std::to_string(getuid()) + "/daemon.sock";
        struct stat st;
        if (stat(sys.c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
            return sys;
    }
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
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);  // wedged daemon: bounded writes too

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
                    rivt::logmsg("rivt: rivtd: %s\n", e.c_str());
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
        rivt::logmsg("rivt: rivtd runs an older protocol, upgrading it in place...\n");
        request_daemon_upgrade(path);
        for (int i = 0; i < 40; i++) {
            usleep(100000);
            out.clear();
            q = query_once(path, out);
            if (q == QueryResult::Ok) return true;
        }
        rivt::logmsg(                "rivt: daemon did not upgrade (too old?) — kill it by pid: pgrep -a rivtd\n");
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
            rivt::logmsg("rivt: cannot create device identity\n");
            return false;
        }
    }
    std::string bundle = net::Identity::authorized_bundle_path();
    dbg("rivt: connecting, racing %zu candidate(s):\n",
            ep.candidates.size());
    for (const auto &c : ep.candidates) {
        // IPv6 candidates are skipped: we run IPv4-only on the wire (see
        // resolve_v6 in quic_engine.cpp), and older rivtd still
        // advertises v6 addresses.
        if (c.host.find(':') != std::string::npos) {
            dbg("rivt:   [%-8s] %s:%u (ipv6, skipped)\n",
                    c.kind.c_str(), c.host.c_str(), c.port);
            continue;
        }
        dbg("rivt:   [%-8s] %s:%u\n", c.kind.c_str(), c.host.c_str(), c.port);
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
        rivt::logmsg("rivt: punch skipped (sig_id=%zu rdv=%zu)\n",
                ep.peer_sig_id.size(), ep.rendezvous.size());
        return;
    }
    dbg("rivt: punch: opening signaling to %.16s...\n", ep.peer_sig_id.c_str());
    m_signaling = net::Signaling::shared(m_loop, *m_identity, ep.rendezvous);
    if (!m_signaling) return;
    // Final give-up for the punched/relayed path: the answer plus a
    // punched handshake comfortably fit in this window; past it the
    // punch has genuinely failed. (probe_failed defers to this timer
    // while the signaling subscription is live.)
    if (m_turn_fallback_timer >= 0) m_loop.remove_timer(m_turn_fallback_timer);
    m_turn_fallback_timer = m_loop.add_timer(30000, [this]() {
        m_loop.remove_timer(m_turn_fallback_timer);
        m_turn_fallback_timer = -1;
        if (m_quic || !m_signaling) return;
        rivt::logmsg("rivt: punch: no punched/relayed path after 30s, giving up\n");
        m_signaling->unsubscribe(m_sig_peer);
        m_signaling = nullptr;
        probe_failed();
    }, true);
    m_sig_peer = ep.peer_sig_id;
    m_signaling->subscribe(m_sig_peer,
        [this](bool answer, std::vector<net::Candidate> c) {
            if (answer) on_answer(c);
        });

    std::string bundle = net::Identity::authorized_bundle_path();
    std::string peer = ep.peer_sig_id;
    // Two engines: [0]=direct, [1]=turn. Add as probes so on_connected
    // adopts them like any candidate.
    auto reflexives = std::make_shared<std::vector<net::Candidate>>();
    auto pending = std::make_shared<int>(2);
    m_punch_stun = {};
    m_punch_turn = {};
    for (int role = 0; role < 2; role++) {
        auto e = net::QuicEngine::create_client(m_loop, *m_identity, bundle);
        if (!e) { (*pending)--; continue; }
        // Punched paths may be high-RTT and lossy (relay + bad cell):
        // give the handshake room, and re-dial the same candidate from
        // the same socket (same relay flow, no budget cost) while the
        // punch window is open.
        e->set_handshake_timeout(12);
        size_t idx = m_probes.size();
        e->on_connected = [this, idx](net::QuicEngine::Conn *) { adopt_probe(idx); };
        e->on_closed = [this, idx, role](net::QuicEngine::Conn *) {
            probe_failed();
            if (m_quic || !m_signaling) return;
            const net::Candidate &t = role == 0 ? m_punch_stun : m_punch_turn;
            if (t.host.empty()) return;
            // Tracked in m_redial_timers: the loop outlives us, and an
            // orphaned one-shot fires into a freed client (seen as an
            // ASan abort when a window closed mid-punch).
            m_redial_timers.push_back(m_loop.add_timer(1500, [this, idx, t]() {
                if (m_quic || !m_signaling || idx >= m_probes.size() || !m_probes[idx])
                    return;
                dbg("rivt: punch: re-dialing [%s] %s:%u", t.kind.c_str(),
                    t.host.c_str(), t.port);
                m_probes[idx]->start_connection(t.host, t.port);
            }, false));
        };
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
                dbg("rivt: punch: sending offer, %zu reflexive candidate(s)\n",
                        reflexives->size());
                m_signaling->send(peer, /*answer=*/false, *reflexives);
                // The shared signaling socket may be silently dead (a
                // network switch kills the TCP without telling us, and a
                // send has no delivery feedback). Probe with whoami — the
                // DO answers on this socket within one RTT — and watch
                // rx: a socket with nothing inbound 2s after the probe is
                // dead, so reconnect it and resend the offer (re-offers
                // are idempotent: the server punches and answers again).
                // A live socket with no answer after 6s means the loss is
                // elsewhere (server signaling, DO routing): resend too.
                m_signaling->probe();
                auto offered = std::make_shared<std::chrono::steady_clock::time_point>(
                    std::chrono::steady_clock::now());
                if (m_offer_retry_timer >= 0) m_loop.remove_timer(m_offer_retry_timer);
                m_offer_retry_timer = m_loop.add_timer(1000, [this, peer, reflexives, offered]() {
                    if (m_quic || !m_signaling) {
                        m_loop.remove_timer(m_offer_retry_timer);
                        m_offer_retry_timer = -1;
                        return;
                    }
                    double since = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - *offered).count();
                    bool dead = since > 2.0 && m_signaling->seconds_since_rx() > since;
                    if (!dead && since <= 6.0) return;
                    rivt::logmsg("rivt: punch: %s, reconnecting signaling and "
                            "resending offer\n",
                            dead ? "signaling socket dead" : "no answer on live socket");
                    m_signaling->restart();
                    m_signaling->send(peer, /*answer=*/false, *reflexives);
                    m_signaling->probe();
                    *offered = std::chrono::steady_clock::now();
                }, true);
            }
        });
    }
}

void RemoteClient::on_answer(const std::vector<net::Candidate> &server_cands) {
    if (m_quic) return;  // already connected via a direct LAN probe
    if (m_offer_retry_timer >= 0) {
        m_loop.remove_timer(m_offer_retry_timer);
        m_offer_retry_timer = -1;
    }
    // m_probes[.. last two] are our [direct, turn] engines (created in
    // begin_punch after the LAN probes).
    net::QuicEngine *direct = nullptr, *turn = nullptr;
    size_t n = m_probes.size();
    if (n >= 2) { direct = m_probes[n - 2].get(); turn = m_probes[n - 1].get(); }
    const net::Candidate *cstun = nullptr, *cturn = nullptr;
    std::vector<net::Candidate> locals;
    dbg("rivt: peer answered with %zu candidate(s):\n", server_cands.size());
    for (const auto &c : server_cands) {
        dbg("rivt:   [%-8s] %s:%u\n", c.kind.c_str(), c.host.c_str(), c.port);
        if (c.kind == "stun" && !cstun) cstun = &c;
        else if (c.kind == "turn" && !cturn) cturn = &c;
        else if (c.kind == "local") locals.push_back(c);
    }
    if (cstun) m_punch_stun = *cstun;
    if (cturn) m_punch_turn = *cturn;
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
    set_link("connected");
    if (m_turn_fallback_timer >= 0) {
        m_loop.remove_timer(m_turn_fallback_timer);
        m_turn_fallback_timer = -1;
    }
    if (m_signaling) { m_signaling->unsubscribe(m_sig_peer); m_signaling = nullptr; }
    m_quic = std::move(m_probes[idx]);
    m_quic_conn = m_quic->client_conn();
    // Losing probes are parked, never destroyed here — engines must not
    // die on any probe's callback stack.
    for (auto &p : m_probes)
        if (p) m_stale_probes.push_back(std::move(p));
    m_probes.clear();

    m_quic->on_connected = nullptr;
    m_quic->on_data = [this](net::QuicEngine::Conn *, uint64_t stream,
                             const uint8_t *d, size_t n) {
        m_qin[stream].append((const char *)d, n);
        process_buffer(m_qin[stream], stream);
    };
    m_quic->on_closed = [this](net::QuicEngine::Conn *) {
        // fail() must see connected()==true to run; close() clears the conn.
        fail();
    };
    if (!m_pending_out.empty()) {
        m_quic->send(m_quic_conn, 0, m_pending_out.data(), m_pending_out.size());
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
    // The punch is still waiting for the peer's answer (its engines have
    // no connection yet, so the loop above can't see them as "trying").
    // The addressed candidates dying must not kill it — over cellular
    // CGNAT they always die and the punched/relayed path is the only one
    // that can work. The punch deadline timer does the final give-up.
    if (m_signaling) return;
    // All candidates failed. Park (we're on one of their stacks).
    for (auto &p : m_probes)
        if (p) m_stale_probes.push_back(std::move(p));
    m_probes.clear();
    m_pending_out.clear();
    if (cert) {
        rivt::logmsg(                "rivt: reached the peer, but its certificate was rejected.\n"
                "      The two devices are not in the same set. Pair them:\n"
                "      on one run `rivt pair` / `rivtd pair`, on the other "
                "`rivt join <code>` / `rivtd join <code>`.\n");
    } else {
        rivt::logmsg(                "rivt: no candidate address responded — the peer's QUIC port "
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
    disarm_ack_probe();
    agent_close_all();
    m_qin.clear();
    m_pane_stream.clear();
    m_in_seq.clear();
    if (m_turn_fallback_timer >= 0) {
        m_loop.remove_timer(m_turn_fallback_timer);
        m_turn_fallback_timer = -1;
    }
    if (m_offer_retry_timer >= 0) {
        m_loop.remove_timer(m_offer_retry_timer);
        m_offer_retry_timer = -1;
    }
    for (int tid : m_redial_timers) m_loop.remove_timer(tid);
    m_redial_timers.clear();
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
    if (m_signaling) { m_signaling->unsubscribe(m_sig_peer); m_signaling = nullptr; }
}

// A dead path (network switch, sleep/wake, expired NAT mapping) shows up
// exactly when the user types: the key goes out, picoquic retransmits the
// unacked data — each retransmit is a probe — and no datagram ever comes
// back. Waiting for the 60s idle timeout freezes the terminal for a
// minute, and punched/relayed paths can't migrate anyway, so declare the
// link dead after 5s of post-send silence and let the controller
// reconnect through a fresh candidate race.
void RemoteClient::arm_ack_probe() {
    if (m_ack_probe_timer >= 0) return;  // already awaiting a reply
    m_await_since = std::chrono::steady_clock::now();
    m_ack_probe_timer = m_loop.add_timer(1000, [this]() {
        if (!m_quic || !m_quic_conn) { disarm_ack_probe(); return; }
        double waited = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_await_since).count();
        if (m_quic->seconds_since_rx() < waited) {
            disarm_ack_probe();  // something arrived since the send
            return;
        }
        if (waited > 5.0) {
            rivt::logmsg("rivt: no reply %.0fs after send, link presumed dead\n", waited);
            disarm_ack_probe();
            fail();
        }
    }, true);
}

void RemoteClient::disarm_ack_probe() {
    if (m_ack_probe_timer < 0) return;
    m_loop.remove_timer(m_ack_probe_timer);
    m_ack_probe_timer = -1;
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
    process_buffer(m_in, /*quic_stream=*/UINT64_MAX);
}

// Parse complete frames out of one reassembly buffer. For QUIC, each
// stream has its own buffer (chunks of different streams interleave);
// pane frames teach us which stream the daemon chose for that pane so
// our input goes back on the same one.
void RemoteClient::process_buffer(std::string &in, uint64_t quic_stream) {
    while (connected() && in.size() >= proto::FRAME_HEADER_SIZE) {
        proto::FrameHeader h;
        if (!proto::decode_frame_header((const uint8_t *)in.data(), h)) {
            rivt::logmsg("rivt: frame desync on stream %llu: len=%u ch=%u type=%u "
                         "(buf %zu bytes) — disconnecting\n",
                         (unsigned long long)quic_stream, h.len, h.channel, h.type,
                         in.size());
            fail();
            return;
        }
        size_t total = proto::FRAME_HEADER_SIZE + h.len;
        if (in.size() < total) return;
        const uint8_t *payload = (const uint8_t *)in.data() + proto::FRAME_HEADER_SIZE;

        if (h.channel != 0 && quic_stream != UINT64_MAX)
            m_pane_stream[h.channel] = quic_stream;
        if (h.channel == 0) {
            dispatch_control(h.type, payload, h.len);
        } else if (h.type == proto::PANE_OUT) {
            auto oit = m_pane_off.find(h.channel);
            if (oit != m_pane_off.end()) oit->second += h.len;
            if (on_output) on_output(h.channel, (const char *)payload, h.len);
        } else if (h.type == proto::PANE_RESUME) {
            if (h.len >= 8) {
                uint64_t off;
                memcpy(&off, payload, 8);
                m_pane_off[h.channel] = off;
                dbg("remote: pane %u stream anchored at %llu", h.channel,
                    (unsigned long long)off);
            }
        } else if (h.type == proto::PANE_SNAPSHOT) {
            // Full state replacement: our offset is void until the
            // daemon's anchor (sent right behind the snapshot) re-bases.
            m_pane_off.erase(h.channel);
            if (on_snapshot) on_snapshot(h.channel, payload, h.len);
        } else if (h.type == proto::PANE_SCROLLBACK) {
            if (on_scrollback) on_scrollback(h.channel, payload, h.len);
        } else if (h.type == proto::PANE_ACK) {
            if (h.len >= 4 && on_pane_ack) {
                uint32_t seq;
                memcpy(&seq, payload, 4);
                bool echo_off = h.len >= 5 && payload[4];
                on_pane_ack(h.channel, seq, echo_off);
            }
        }
        in.erase(0, total);
    }
}

void RemoteClient::dispatch_control(uint16_t type, const uint8_t *data, size_t len) {
    proto::Reader r(data, len);
    switch ((MsgType)type) {
    case MsgType::HelloOk: {
        uint32_t ver = r.u32();
        uint32_t epoch = r.remaining() >= 4 ? r.u32() : 0;
        if (epoch != m_daemon_epoch) {
            // Different daemon lifetime: our pane offsets are meaningless.
            m_pane_off.clear();
            m_daemon_epoch = epoch;
        }
        if (r.ok && ver == proto::PROTO_VERSION) {
            if (on_hello_ok) on_hello_ok();
        } else {
            rivt::logmsg(                    "rivt: rivtd protocol version mismatch (daemon %u, client %u) — "
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
    case MsgType::AgentOpen: {
        uint32_t id = r.u32();
        if (r.ok) agent_open(id);
        break;
    }
    case MsgType::AgentData: {
        uint32_t id = r.u32();
        if (!r.ok) break;
        auto it = m_agent.find(id);
        if (it == m_agent.end()) break;
        size_t n = r.remaining();
        it->second.out.append((const char *)data + (len - n), n);
        agent_flush(it->second, id);
        break;
    }
    case MsgType::AgentClose: {
        uint32_t id = r.u32();
        if (r.ok) agent_close(id, false);
        break;
    }
    default:
        break;
    }
}

// ---------------- SSH agent forwarding ----------------

void RemoteClient::agent_open(uint32_t id) {
    const char *sock = getenv("SSH_AUTH_SOCK");
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (!sock || !*sock || strlen(sock) >= sizeof(addr.sun_path)) {
        dbg("agent: no local SSH_AUTH_SOCK — refusing stream %u", id);
        agent_close(id, true);
        return;
    }
    strcpy(addr.sun_path, sock);
    int fd = net::socket_cloexec(AF_UNIX, SOCK_STREAM, 0, /*nonblock=*/true);
    if (fd < 0 || (::connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 &&
                   errno != EINPROGRESS)) {
        dbg("agent: cannot connect %s (errno %d) — refusing stream %u",
            sock, errno, id);
        if (fd >= 0) ::close(fd);
        agent_close(id, true);
        return;
    }
    m_agent[id] = {fd, {}, 0, false};
    m_loop.add_fd(fd, [this, id](uint32_t ev) { agent_event(id, ev); });
    dbg("agent: stream %u bridged to %s", id, sock);
}

void RemoteClient::agent_event(uint32_t id, uint32_t ev) {
    auto it = m_agent.find(id);
    if (it == m_agent.end()) return;
    if (ev & EV_WRITE) agent_flush(it->second, id);
    if (ev & EV_READ) {
        uint8_t buf[4096];
        for (;;) {
            ssize_t n = recv(it->second.fd, buf, sizeof buf, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                agent_close(id, true);
                return;
            }
            if (n == 0) {
                agent_close(id, true);
                return;
            }
            proto::Writer w;
            w.u32(id);
            w.bytes(buf, (size_t)n);
            send_control((uint16_t)MsgType::AgentData, w);
        }
    }
    if (ev & (EV_HUP | EV_ERR)) agent_close(id, true);
}

void RemoteClient::agent_flush(AgentBridge &b, uint32_t id) {
    (void)id;
    while (b.out_off < b.out.size()) {
        ssize_t n = ::send(b.fd, b.out.data() + b.out_off, b.out.size() - b.out_off, 0);
        if (n > 0) { b.out_off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        break;  // hard error surfaces as HUP on the next event
    }
    if (b.out_off == b.out.size()) {
        b.out.clear();
        b.out_off = 0;
    }
    bool want = b.out_off < b.out.size();
    if (want != b.write_armed) {
        b.write_armed = want;
        m_loop.modify_fd(b.fd, EV_READ | (want ? EV_WRITE : 0));
    }
}

void RemoteClient::agent_close(uint32_t id, bool notify) {
    auto it = m_agent.find(id);
    if (it != m_agent.end()) {
        m_loop.remove_fd(it->second.fd);
        ::close(it->second.fd);
        m_agent.erase(it);
    }
    if (notify && connected()) {
        proto::Writer w;
        w.u32(id);
        send_control((uint16_t)MsgType::AgentClose, w);
    }
}

void RemoteClient::agent_close_all() {
    for (auto &[id, b] : m_agent) {
        m_loop.remove_fd(b.fd);
        ::close(b.fd);
    }
    m_agent.clear();
}

void RemoteClient::send_frame(uint16_t channel, uint16_t type, const void *data, size_t len) {
    uint8_t hdr[proto::FRAME_HEADER_SIZE];
    proto::encode_frame_header(hdr, {(uint32_t)len, channel, type});
    if (m_quic_conn) {
        uint64_t stream = 0;
        if (channel != 0) {
            auto it = m_pane_stream.find(channel);
            if (it != m_pane_stream.end()) stream = it->second;
            // Unknown yet (input before any output): stream 0 is always
            // correct — framing is self-describing — just unprioritized.
        }
        m_quic->send(m_quic_conn, stream, hdr, sizeof hdr);
        if (len) m_quic->send(m_quic_conn, stream, data, len);
        arm_ack_probe();
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

void RemoteClient::verify_link() {
    if (!m_quic_conn) return;
    proto::Writer w;
    send_control((uint16_t)MsgType::Ping, w);  // send_frame arms the ACK probe
}

void RemoteClient::resize_pane(uint32_t pane_id, int dx, int dy) {
    proto::Writer w;
    w.u32(pane_id);
    w.i32(dx);
    w.i32(dy);
    send_control((uint16_t)MsgType::ResizePane, w);
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
    // Resume entries: stale panes (other sessions, dead panes) are
    // simply ignored daemon-side; wrong-epoch offsets never exist here
    // (cleared on HelloOk).
    for (const auto &[pane, off] : m_pane_off) {
        w.u32(pane);
        w.u64(off);
    }
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

uint32_t RemoteClient::send_input(uint32_t pane_id, const char *data, size_t len) {
    uint32_t seq = ++m_in_seq[pane_id];
    std::string payload;
    payload.reserve(4 + len);
    payload.append((const char *)&seq, 4);
    payload.append(data, len);
    send_frame((uint16_t)pane_id, proto::PANE_IN, payload.data(), payload.size());
    return seq;
}

} // namespace rivt
