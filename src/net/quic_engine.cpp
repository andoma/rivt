#include "net/quic_engine.h"
#include "core/debug.h"
#include "net/identity.h"
#include "net/turn.h"
#include "net/sock.h"

#include <picoquic.h>
#include <picoquic_utils.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rivt::net {

static bool qdbg() {
    static int v = getenv("RIVT_QUIC_DEBUG") ? 1 : 0;
    return v;
}

// Link impairment simulator (client-side testing: predictive echo, bad
// cell coverage, metro tunnels). RIVT_NETEM applies to every datagram
// in both directions on this engine's socket:
//   RIVT_NETEM="delay=300,jitter=200,loss=10,outage=8/45"
//     delay ms base one-way, jitter ms uniform, loss %, and an
//     outage of N seconds every M seconds (total blackout).
// Presets: RIVT_NETEM=metro (grim), cell (mediocre), edge (awful).
struct Netem {
    bool enabled = false;
    int delay_ms = 0, jitter_ms = 0;
    double loss = 0.0;
    int outage_s = 0, outage_period_s = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    Netem() {
        const char *spec = getenv("RIVT_NETEM");
        if (!spec || !*spec) return;
        enabled = true;
        std::string v = spec;
        if (v == "metro")     v = "delay=250,jitter=350,loss=12,outage=10/40";
        else if (v == "cell") v = "delay=120,jitter=60,loss=3";
        else if (v == "edge") v = "delay=500,jitter=400,loss=20,outage=15/60";
        auto num = [&](const char *key) -> int {
            auto p = v.find(std::string(key) + "=");
            return p == std::string::npos ? 0 : atoi(v.c_str() + p + strlen(key) + 1);
        };
        delay_ms = num("delay");
        jitter_ms = num("jitter");
        loss = num("loss") / 100.0;
        auto o = v.find("outage=");
        if (o != std::string::npos) {
            outage_s = atoi(v.c_str() + o + 7);
            auto sl = v.find('/', o);
            if (sl != std::string::npos) outage_period_s = atoi(v.c_str() + sl + 1);
        }
        rivt::logmsg("netem: ACTIVE delay=%dms jitter=%dms loss=%.0f%% outage=%ds/%ds\n",
                     delay_ms, jitter_ms, loss * 100, outage_s, outage_period_s);
    }

    bool in_outage() const {
        if (!outage_s || !outage_period_s) return false;
        double t = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - start).count();
        double phase = t - (int)(t / outage_period_s) * (double)outage_period_s;
        return phase < outage_s;
    }
    // <0 = drop; otherwise extra one-way delay in ms.
    int impair() {
        if (!enabled) return 0;
        if (in_outage()) return -1;
        if (loss > 0 && (rand() / (double)RAND_MAX) < loss) return -1;
        int j = jitter_ms ? rand() % (2 * jitter_ms + 1) - jitter_ms : 0;
        int d = delay_ms + j;
        return d > 0 ? d : 0;
    }
    static Netem &instance() {
        static Netem n;
        return n;
    }
};

static constexpr char ALPN[] = "rivt/1";
static constexpr uint64_t CONTROL_STREAM = 0;  // client's first bidi stream

// C trampoline: callback_ctx is the engine for brand-new inbound
// connections (quic default ctx) and the Conn afterwards.
static int engine_stream_cb(picoquic_cnx_t *cnx, uint64_t stream_id, uint8_t *bytes,
                            size_t length, picoquic_call_back_event_t event,
                            void *callback_ctx, void *stream_ctx) {
    (void)stream_ctx;
    void *def = picoquic_get_default_callback_context(picoquic_get_quic_ctx(cnx));
    QuicEngine *self;
    QuicEngine::Conn *conn;
    if (callback_ctx == def) {
        self = (QuicEngine *)callback_ctx;
        conn = self->adopt(cnx);
    } else {
        conn = (QuicEngine::Conn *)callback_ctx;
        self = conn->engine;
    }
    return self->handle_event(cnx, conn, stream_id, bytes, length, (int)event);
}

std::unique_ptr<QuicEngine> QuicEngine::listen(EventLoop &loop, uint16_t port,
                                               const Identity &id,
                                               const std::string &bundle) {
    auto e = std::unique_ptr<QuicEngine>(new QuicEngine(loop));
    if (!e->init(port, id, bundle, true)) return nullptr;
    return e;
}

std::unique_ptr<QuicEngine> QuicEngine::create_client(EventLoop &loop, const Identity &id,
                                                      const std::string &bundle) {
    auto e = std::unique_ptr<QuicEngine>(new QuicEngine(loop));
    if (!e->init(0, id, bundle, false)) return nullptr;
    return e;
}

// Resolve IPv4 only, into a v4-mapped v6 sockaddr for our dual-stack
// socket. We never speak IPv6 on the wire: on typical eyeball networks a
// host often has v6 addresses but no working v6 route, so sends succeed
// and replies never come — the connection blackholes instead of failing
// fast. (AI_V4MAPPED with AF_INET6 also returns native v6 first, which
// is how STUN used to pick a dead address.)
static bool resolve_v6(const std::string &host, uint16_t port, struct sockaddr_storage *out,
                       socklen_t *outlen) {
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return false;
    auto *v4 = (const struct sockaddr_in *)res->ai_addr;
    struct sockaddr_in6 sa {};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = v4->sin_port;
    sa.sin6_addr.s6_addr[10] = 0xff;
    sa.sin6_addr.s6_addr[11] = 0xff;
    memcpy(&sa.sin6_addr.s6_addr[12], &v4->sin_addr, 4);
    freeaddrinfo(res);
    memcpy(out, &sa, sizeof sa);
    *outlen = sizeof sa;
    return true;
}

bool QuicEngine::start_connection(const std::string &host, uint16_t port) {
    struct sockaddr_storage ss {};
    socklen_t sl = 0;
    if (!resolve_v6(host, port, &ss, &sl)) {
        rivt::logmsg("rivt: cannot resolve %s\n", host.c_str());
        return false;
    }
    m_label = host + ":" + std::to_string(port);
    // No SNI: identity is the pinned certificate, not a hostname.
    picoquic_cnx_t *cnx = picoquic_create_cnx(
        m_quic, picoquic_null_connection_id, picoquic_null_connection_id,
        (struct sockaddr *)&ss, picoquic_current_time(), 0, nullptr, ALPN, 1);
    if (!cnx) return false;
    m_client_conn = adopt(cnx);
    if (picoquic_start_client_cnx(cnx) != 0) return false;
    pump();
    return true;
}

std::unique_ptr<QuicEngine> QuicEngine::connect(EventLoop &loop, const std::string &host,
                                                uint16_t port, const Identity &id,
                                                const std::string &bundle) {
    auto e = create_client(loop, id, bundle);
    if (!e || !e->start_connection(host, port)) return nullptr;
    return e;
}

void QuicEngine::punch(const std::string &host, uint16_t port) {
    struct sockaddr_storage ss {};
    socklen_t sl = 0;
    if (!resolve_v6(host, port, &ss, &sl)) return;
    // A tiny non-QUIC datagram: opens the NAT mapping; the peer's stack
    // discards it (fixed-bit clear, not a STUN cookie either).
    const uint8_t p[1] = {0x00};
    for (int i = 0; i < 3; i++)
        sendto(m_fd, p, sizeof p, 0, (struct sockaddr *)&ss, sl);
}

QuicEngine::~QuicEngine() {
    // picoquic_free tears down connections, which emits close events;
    // during destruction the owner is (partially) gone, so no user
    // callbacks may fire.
    on_connected = nullptr;
    on_closed = nullptr;
    on_data = nullptr;
    if (m_stun_timer >= 0) m_loop.remove_timer(m_stun_timer);
    if (m_timer >= 0) m_loop.remove_timer(m_timer);
    if (m_fd >= 0) {
        m_loop.remove_fd(m_fd);
        ::close(m_fd);
    }
    if (m_quic) picoquic_free(m_quic);
}

bool QuicEngine::init(uint16_t bind_port, const Identity &id, const std::string &bundle,
                      bool server) {
    m_quic = picoquic_create(64, id.cert_path().c_str(), id.key_path().c_str(),
                             bundle.c_str(), ALPN, engine_stream_cb, this, nullptr,
                             nullptr, nullptr, picoquic_current_time(), nullptr,
                             nullptr, nullptr, 0);
    if (!m_quic) {
        rivt::logmsg("rivt: picoquic_create failed (identity/bundle unreadable?)\n");
        return false;
    }
    // Mutual auth: both sides present certs, both validate against the
    // authorized bundle (self-signed peer certs are their own roots).
    picoquic_set_client_authentication(m_quic, 1);
    picoquic_disable_port_blocking(m_quic, 1);
    // Terminal sessions idle for long stretches; keep-alives (enabled
    // per connection, defaulting to idle/2) must fit inside this.
    uint64_t idle_ms = 60000;
    if (const char *e = getenv("RIVT_QUIC_IDLE_MS")) idle_ms = strtoull(e, nullptr, 10);
    picoquic_set_default_idle_timeout(m_quic, idle_ms);
    // Fail unreachable peers quickly (reconnect loops depend on this;
    // picoquic's default is ~30 s).
    picoquic_set_default_handshake_timeout(m_quic, 5ull * 1000000);

    m_fd = socket_cloexec(AF_INET6, SOCK_DGRAM, 0, /*nonblock=*/true);
    if (m_fd < 0) return false;
    int off = 0;
    setsockopt(m_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);  // dual stack
    // Bulk terminal output is bursty; default UDP buffers overflow and
    // silently drop, which QUIC pays for in loss recovery.
    int bufsz = 4 << 20;
    setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
    setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
    struct sockaddr_in6 sa {};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(bind_port);
    if (bind(m_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("rivt: quic bind");
        return false;
    }
    socklen_t sl = sizeof m_local;
    getsockname(m_fd, (struct sockaddr *)&m_local, &sl);

    m_loop.add_fd(m_fd, [this](uint32_t ev) { on_socket(ev); });
    m_timer = m_loop.add_timer(1000, [this]() { pump(); }, true);
    (void)server;
    return true;
}

QuicEngine::Conn *QuicEngine::adopt(picoquic_cnx_t *cnx) {
    auto conn = std::make_unique<Conn>();
    conn->cnx = cnx;
    conn->engine = this;
    Conn *raw = conn.get();
    m_conns.push_back(std::move(conn));
    picoquic_set_callback(cnx, engine_stream_cb, raw);
    picoquic_enable_keep_alive(cnx, 0);  // idle_timeout / 2
    return raw;
}

void QuicEngine::mark_closed(Conn *c) {
    if (c->dead) return;
    c->dead = true;
    if (c->cnx)
        dbg("quic: connection closed (state=%d local_err=0x%lx remote_err=0x%lx)%s",
            (int)picoquic_get_cnx_state(c->cnx),
            (unsigned long)picoquic_get_local_error(c->cnx),
            (unsigned long)picoquic_get_remote_error(c->cnx),
            c->established ? "" : " [never established]");
    if (on_closed) on_closed(c);
}

int QuicEngine::handle_event(picoquic_cnx_t *cnx, Conn *conn, uint64_t stream_id,
                             uint8_t *bytes, size_t length, int event) {
    if (qdbg())
        rivt::logmsg("quic[%p] event=%d stream=%lu len=%zu state=%d lerr=0x%lx rerr=0x%lx\n",
                (void *)this, event, (unsigned long)stream_id, length,
                (int)picoquic_get_cnx_state(cnx),
                (unsigned long)picoquic_get_local_error(cnx),
                (unsigned long)picoquic_get_remote_error(cnx));
    switch ((picoquic_call_back_event_t)event) {
    case picoquic_callback_ready:
        conn->established = true;
        if (on_connected) on_connected(conn);
        break;
    case picoquic_callback_prepare_to_send: {
        // The stack can send on this stream: feed it from that stream's
        // buffer. 'bytes' is the opaque buffer context, 'length' the max.
        auto it = conn->streams.find(stream_id);
        if (it == conn->streams.end()) {
            picoquic_provide_stream_data_buffer(bytes, 0, 0, 0);
            break;
        }
        Conn::StreamBuf &sb = it->second;
        size_t avail = sb.queued();
        size_t chunk = avail < length ? avail : length;
        bool before_high = conn->queued() >= SEND_LOW_WATER;
        uint8_t *dst = picoquic_provide_stream_data_buffer(bytes, chunk, 0,
                                                           chunk < avail);
        if (dst && chunk > 0) {
            memcpy(dst, sb.out.data() + sb.out_off, chunk);
            sb.out_off += chunk;
            if (sb.out_off == sb.out.size()) {
                sb.out.clear();
                sb.out_off = 0;
            }
        }
        if (before_high && conn->queued() < SEND_LOW_WATER && on_drained && !conn->dead)
            on_drained(conn);
        break;
    }
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (length > 0 && on_data && !conn->dead)
            on_data(conn, stream_id, bytes, length);
        // Fin on the control stream ends the connection; a pane stream
        // finishing is just that pane going away.
        if (event == picoquic_callback_stream_fin && stream_id == CONTROL_STREAM)
            mark_closed(conn);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
        // 0x12e = TLS alert 46 (certificate_unknown): reached the peer
        // but the cert isn't trusted.
        if (picoquic_get_local_error(cnx) == 0x12e ||
            picoquic_get_remote_error(cnx) == 0x12e)
            m_cert_rejected = true;
        // Close reasons of real connections are the one thing post-mortem
        // debugging always needs — log those unconditionally. Raced
        // candidates that never established close by the handful on every
        // connect; those go under --debug.
        if (conn->established)
            rivt::logmsg("quic: %s closed (established, ev=%d lerr=0x%lx rerr=0x%lx, "
                    "last rx %.1fs ago)\n",
                    m_label.empty() ? "conn" : m_label.c_str(), event,
                    (unsigned long)picoquic_get_local_error(cnx),
                    (unsigned long)picoquic_get_remote_error(cnx), seconds_since_rx());
        else
            dbg("quic: %s closed (never established, ev=%d lerr=0x%lx rerr=0x%lx, "
                "last rx %.1fs ago)",
                m_label.empty() ? "conn" : m_label.c_str(), event,
                (unsigned long)picoquic_get_local_error(cnx),
                (unsigned long)picoquic_get_remote_error(cnx), seconds_since_rx());
        mark_closed(conn);
        picoquic_set_callback(cnx, nullptr, nullptr);
        break;
    default:
        break;
    }
    return 0;
}

void QuicEngine::send(Conn *c, uint64_t stream, const void *data, size_t len) {
    if (!c || c->dead) return;
    c->streams[stream].out.append((const char *)data, len);
    // Marking an unopened stream id implicitly opens it (ids must obey
    // the initiator's numbering: we allocate server-side pane streams as
    // 1,5,9,... on the daemon; the client only reuses ids it has seen).
    picoquic_mark_active_stream(c->cnx, stream, 1, nullptr);
    pump();
}

void QuicEngine::close_conn(Conn *c) {
    if (!c || c->dead) return;
    picoquic_close(c->cnx, 0);
    pump();
}

void QuicEngine::on_socket(uint32_t events) {
    if (events & EV_WRITE) {
        m_want_write = false;
        m_loop.modify_fd(m_fd, EV_READ);
        pump();
    }
    if (!(events & EV_READ)) return;
    uint8_t buf[65536];
    for (;;) {
        struct sockaddr_storage from {};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(m_fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        if (n == 0) continue;
        m_last_rx_us = picoquic_current_time();
        if (m_stun_active && is_stun(buf, (size_t)n)) {
            struct sockaddr_storage mapped {};
            if (stun_parse_response(buf, (size_t)n, m_stun_txid, &mapped)) {
                m_stun_active = false;
                if (m_stun_timer >= 0) { m_loop.remove_timer(m_stun_timer); m_stun_timer = -1; }
                auto cb = m_stun_cb;
                m_stun_cb = nullptr;
                if (cb) cb(true, mapped);
            }
            continue;  // never hand STUN to picoquic
        }
        if (qdbg()) rivt::logmsg("quic[%p] rx %zd\n", (void *)this, n);
        int impair = Netem::instance().impair();
        if (impair < 0) continue;  // netem: dropped
        if (impair > 0) {
            std::string pkt((const char *)buf, n);
            struct sockaddr_storage from_copy = from;
            m_loop.add_timer(impair, [this, pkt, from_copy]() {
                struct sockaddr_storage f = from_copy;
                picoquic_incoming_packet(m_quic, (uint8_t *)pkt.data(), pkt.size(),
                                         (struct sockaddr *)&f,
                                         (struct sockaddr *)&m_local, 0, 0,
                                         picoquic_current_time());
                pump();
            }, false);
            continue;
        }
        picoquic_incoming_packet(m_quic, buf, (size_t)n, (struct sockaddr *)&from,
                                 (struct sockaddr *)&m_local, 0, 0,
                                 picoquic_current_time());
    }
    pump();
}

double QuicEngine::seconds_since_rx() const {
    if (m_last_rx_us == 0) return 0.0;
    uint64_t now = picoquic_current_time();
    return now > m_last_rx_us ? (now - m_last_rx_us) / 1e6 : 0.0;
}

uint16_t QuicEngine::local_port() const {
    if (m_local.ss_family == AF_INET6)
        return ntohs(((struct sockaddr_in6 *)&m_local)->sin6_port);
    if (m_local.ss_family == AF_INET)
        return ntohs(((struct sockaddr_in *)&m_local)->sin_port);
    return 0;
}

void QuicEngine::stun_send() {
    uint8_t req[20];
    stun_build_request(req, m_stun_txid);
    socklen_t sl = m_stun_server.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                       : sizeof(struct sockaddr_in);
    sendto(m_fd, req, sizeof req, 0, (struct sockaddr *)&m_stun_server, sl);
}

void QuicEngine::discover_reflexive(
    std::function<void(bool, const struct sockaddr_storage &)> cb,
    const char *stun_host, uint16_t stun_port) {
    if (m_stun_active) { struct sockaddr_storage z {}; cb(false, z); return; }
    const char *host = stun_host ? stun_host : "stun.cloudflare.com";
    socklen_t sl = 0;
    if (!resolve_v6(host, stun_port, &m_stun_server, &sl)) {
        rivt::logmsg("rivt: stun: cannot resolve %s\n", host);
        struct sockaddr_storage z {};
        cb(false, z);
        return;
    }

    m_stun_active = true;
    m_stun_tries = 0;
    m_stun_cb = std::move(cb);
    stun_send();
    // Retransmit a few times (UDP), then give up.
    m_stun_timer = m_loop.add_timer(500, [this]() {
        if (!m_stun_active) return;
        if (++m_stun_tries >= 5) {
            m_stun_active = false;
            m_loop.remove_timer(m_stun_timer);
            m_stun_timer = -1;
            auto cb = m_stun_cb;
            m_stun_cb = nullptr;
            struct sockaddr_storage z {};
            if (cb) cb(false, z);
            return;
        }
        stun_send();
    }, true);
}

void QuicEngine::add_turn(TurnRelay *turn) {
    turn->on_data = [this, turn](const struct sockaddr_in &peer, const uint8_t *d, size_t n) {
        feed_relayed(turn, peer, d, n);
    };
}

void QuicEngine::remove_turn(TurnRelay *turn) {
    std::erase_if(m_relayed_peers, [turn](const auto &e) { return e.second == turn; });
}

void QuicEngine::feed_relayed(TurnRelay *turn, const struct sockaddr_in &peer,
                              const uint8_t *d, size_t n) {
    m_last_rx_us = picoquic_current_time();
    // Remember which relay this peer arrived through so pump() routes
    // replies back the same way (a reconnecting peer may show up via a
    // newer relay: update the mapping).
    bool known = false;
    for (auto &e : m_relayed_peers)
        if (e.first.sin_addr.s_addr == peer.sin_addr.s_addr &&
            e.first.sin_port == peer.sin_port) {
            e.second = turn;
            known = true;
            break;
        }
    if (!known) m_relayed_peers.push_back({peer, turn});
    picoquic_incoming_packet(m_quic, (uint8_t *)d, n, (struct sockaddr *)&peer,
                             (struct sockaddr *)&m_local, 0, 0, picoquic_current_time());
    pump();
}

TurnRelay *QuicEngine::relay_for(const struct sockaddr_storage &to,
                                 struct sockaddr_in *peer) const {
    if (to.ss_family != AF_INET) return nullptr;
    auto *s = (const struct sockaddr_in *)&to;
    for (const auto &e : m_relayed_peers)
        if (e.first.sin_addr.s_addr == s->sin_addr.s_addr &&
            e.first.sin_port == s->sin_port) {
            *peer = *s;
            return e.second;
        }
    return nullptr;
}

void QuicEngine::pump() {
    uint8_t buf[1536];
    for (;;) {
        struct sockaddr_storage to {}, fromaddr {};
        int if_index = 0;
        size_t send_len = 0;
        picoquic_cnx_t *last = nullptr;
        int rc = picoquic_prepare_next_packet(m_quic, picoquic_current_time(), buf,
                                              sizeof buf, &send_len, &to, &fromaddr,
                                              &if_index, nullptr, &last);
        if (rc != 0 || send_len == 0) break;
        struct sockaddr_in rpeer {};
        int impair = Netem::instance().impair();
        if (impair < 0) continue;  // netem: dropped
        if (TurnRelay *tr = relay_for(to, &rpeer)) {
            if (impair > 0) {
                std::string pkt((const char *)buf, send_len);
                struct sockaddr_in peer_copy = rpeer;
                m_loop.add_timer(impair, [tr, peer_copy, pkt]() {
                    tr->send_to(peer_copy, (const uint8_t *)pkt.data(), pkt.size());
                }, false);
                continue;
            }
            tr->send_to(rpeer, buf, send_len);
            continue;
        }
        if (impair > 0) {
            std::string pkt((const char *)buf, send_len);
            struct sockaddr_storage to_copy = to;
            int fd = m_fd;
            socklen_t sl = to.ss_family == AF_INET ? sizeof(struct sockaddr_in)
                                                   : sizeof(struct sockaddr_in6);
            m_loop.add_timer(impair, [fd, to_copy, pkt, sl]() {
                sendto(fd, pkt.data(), pkt.size(), 0,
                       (const struct sockaddr *)&to_copy, sl);
            }, false);
            continue;
        }
        ssize_t sent = sendto(m_fd, buf, send_len, 0, (struct sockaddr *)&to,
               to.ss_family == AF_INET ? sizeof(struct sockaddr_in)
                                       : sizeof(struct sockaddr_in6));
        if (qdbg()) rivt::logmsg("quic[%p] tx %zu -> %zd (fam %d)\n",
                            (void *)this, send_len, sent, to.ss_family);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Kernel buffer full: this packet is lost to us (QUIC will
            // retransmit), stop bursting and resume when writable.
            if (!m_want_write) {
                m_want_write = true;
                m_loop.modify_fd(m_fd, EV_READ | EV_WRITE);
            }
            break;
        }
    }
    int64_t delay_us = picoquic_get_next_wake_delay(m_quic, picoquic_current_time(),
                                                    60ll * 1000000);
    m_loop.reset_timer(m_timer, (int)(delay_us / 1000) + 1);
}

} // namespace rivt::net
