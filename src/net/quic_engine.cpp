#include "net/quic_engine.h"
#include "core/debug.h"
#include "net/identity.h"
#include "net/turn.h"
#include "net/sock.h"

#include <picoquic.h>
#include <picoquic_utils.h>

#include <cstring>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rivt::net {

static bool qdbg() {
    static int v = getenv("RIVT_QUIC_DEBUG") ? 1 : 0;
    return v;
}

static constexpr char ALPN[] = "rivt/1";
static constexpr uint64_t STREAM_ID = 0;  // client's first bidi stream

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

// Resolve to a v4-mapped v6 sockaddr for our dual-stack socket.
static bool resolve_v6(const std::string &host, uint16_t port, struct sockaddr_storage *out,
                       socklen_t *outlen) {
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_V4MAPPED | AI_ALL;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return false;
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *outlen = res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

bool QuicEngine::start_connection(const std::string &host, uint16_t port) {
    struct sockaddr_storage ss {};
    socklen_t sl = 0;
    if (!resolve_v6(host, port, &ss, &sl)) {
        fprintf(stderr, "rivt: cannot resolve %s\n", host.c_str());
        return false;
    }
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
        fprintf(stderr, "rivt: picoquic_create failed (identity/bundle unreadable?)\n");
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
        fprintf(stderr, "quic[%p] event=%d stream=%lu len=%zu state=%d lerr=0x%lx rerr=0x%lx\n",
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
        // The stack can send on our stream: feed it from conn->out.
        // 'bytes' is the opaque buffer context, 'length' the max size.
        size_t avail = conn->queued();
        size_t chunk = avail < length ? avail : length;
        bool before_high = conn->queued() >= SEND_LOW_WATER;
        uint8_t *dst = picoquic_provide_stream_data_buffer(bytes, chunk, 0,
                                                           chunk < avail);
        if (dst && chunk > 0) {
            memcpy(dst, conn->out.data() + conn->out_off, chunk);
            conn->out_off += chunk;
            if (conn->out_off == conn->out.size()) {
                conn->out.clear();
                conn->out_off = 0;
            }
        }
        if (before_high && conn->queued() < SEND_LOW_WATER && on_drained && !conn->dead)
            on_drained(conn);
        break;
    }
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (stream_id == STREAM_ID && length > 0 && on_data && !conn->dead)
            on_data(conn, bytes, length);
        if (event == picoquic_callback_stream_fin) mark_closed(conn);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
        // 0x12e = TLS alert 46 (certificate_unknown): reached the peer
        // but the cert isn't trusted.
        if (picoquic_get_local_error(cnx) == 0x12e ||
            picoquic_get_remote_error(cnx) == 0x12e)
            m_cert_rejected = true;
        mark_closed(conn);
        picoquic_set_callback(cnx, nullptr, nullptr);
        break;
    default:
        break;
    }
    return 0;
}

void QuicEngine::send(Conn *c, const void *data, size_t len) {
    if (!c || c->dead) return;
    c->out.append((const char *)data, len);
    picoquic_mark_active_stream(c->cnx, STREAM_ID, 1, nullptr);
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
        if (qdbg()) fprintf(stderr, "quic[%p] rx %zd\n", (void *)this, n);
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
    // Resolve into a v4-mapped v6 address to match our dual-stack socket.
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_V4MAPPED | AI_ALL;
    hints.ai_socktype = SOCK_DGRAM;
    const char *host = stun_host ? stun_host : "stun.cloudflare.com";
    if (getaddrinfo(host, std::to_string(stun_port).c_str(), &hints, &res) != 0 || !res) {
        struct sockaddr_storage z {};
        cb(false, z);
        return;
    }
    memcpy(&m_stun_server, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

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

void QuicEngine::enable_turn(TurnRelay *turn) {
    m_turn = turn;
    turn->on_data = [this](const struct sockaddr_in &peer, const uint8_t *d, size_t n) {
        feed_relayed(peer, d, n);
    };
}

void QuicEngine::feed_relayed(const struct sockaddr_in &peer, const uint8_t *d, size_t n) {
    m_last_rx_us = picoquic_current_time();
    // Remember this peer so pump() routes replies back through TURN.
    bool known = false;
    for (auto &p : m_relayed_peers)
        if (p.sin_addr.s_addr == peer.sin_addr.s_addr && p.sin_port == peer.sin_port) {
            known = true; break;
        }
    if (!known) m_relayed_peers.push_back(peer);
    picoquic_incoming_packet(m_quic, (uint8_t *)d, n, (struct sockaddr *)&peer,
                             (struct sockaddr *)&m_local, 0, 0, picoquic_current_time());
    pump();
}

bool QuicEngine::is_relayed(const struct sockaddr_storage &to, struct sockaddr_in *peer) const {
    if (to.ss_family != AF_INET) return false;
    auto *s = (const struct sockaddr_in *)&to;
    for (const auto &p : m_relayed_peers)
        if (p.sin_addr.s_addr == s->sin_addr.s_addr && p.sin_port == s->sin_port) {
            *peer = *s; return true;
        }
    return false;
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
        if (m_turn && is_relayed(to, &rpeer)) {
            m_turn->send_to(rpeer, buf, send_len);
            continue;
        }
        ssize_t sent = sendto(m_fd, buf, send_len, 0, (struct sockaddr *)&to,
               to.ss_family == AF_INET ? sizeof(struct sockaddr_in)
                                       : sizeof(struct sockaddr_in6));
        if (qdbg()) fprintf(stderr, "quic[%p] tx %zu -> %zd (fam %d)\n",
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
