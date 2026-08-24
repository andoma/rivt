#include "net/quic_engine.h"
#include "core/debug.h"
#include "net/identity.h"

#include <picoquic.h>
#include <picoquic_utils.h>

#include <cstring>
#include <netdb.h>
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

std::unique_ptr<QuicEngine> QuicEngine::connect(EventLoop &loop, const std::string &host,
                                                uint16_t port, const Identity &id,
                                                const std::string &bundle) {
    auto e = std::unique_ptr<QuicEngine>(new QuicEngine(loop));
    if (!e->init(0, id, bundle, false)) return nullptr;

    // Our socket is dual-stack IPv6; map v4 targets into v6 space so
    // sendto() on the AF_INET6 socket accepts them.
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_V4MAPPED;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        fprintf(stderr, "rivt: cannot resolve %s\n", host.c_str());
        return nullptr;
    }
    // No SNI: identity is the pinned certificate, not a hostname, and
    // picotls enforces hostname/IP checks whenever an SNI is present.
    picoquic_cnx_t *cnx = picoquic_create_cnx(
        e->m_quic, picoquic_null_connection_id, picoquic_null_connection_id,
        res->ai_addr, picoquic_current_time(), 0, nullptr, ALPN, 1);
    freeaddrinfo(res);
    if (!cnx) return nullptr;

    Conn *conn = e->adopt(cnx);
    e->m_client_conn = conn;
    if (picoquic_start_client_cnx(cnx) != 0) return nullptr;
    e->pump();
    return e;
}

QuicEngine::~QuicEngine() {
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

    m_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int off = 0;
    setsockopt(m_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);  // dual stack
    if (m_fd < 0) return false;
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
    return raw;
}

void QuicEngine::mark_closed(Conn *c) {
    if (c->dead) return;
    c->dead = true;
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
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        if (stream_id == STREAM_ID && length > 0 && on_data && !conn->dead)
            on_data(conn, bytes, length);
        if (event == picoquic_callback_stream_fin) mark_closed(conn);
        break;
    case picoquic_callback_close:
    case picoquic_callback_application_close:
    case picoquic_callback_stateless_reset:
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
    picoquic_add_to_stream(c->cnx, STREAM_ID, (const uint8_t *)data, len, 0);
    pump();
}

void QuicEngine::close_conn(Conn *c) {
    if (!c || c->dead) return;
    picoquic_close(c->cnx, 0);
    pump();
}

void QuicEngine::on_socket(uint32_t events) {
    if (!(events & EV_READ)) return;
    uint8_t buf[65536];
    for (;;) {
        struct sockaddr_storage from {};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(m_fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) break;
        if (n == 0) continue;
        if (qdbg()) fprintf(stderr, "quic[%p] rx %zd\n", (void *)this, n);
        picoquic_incoming_packet(m_quic, buf, (size_t)n, (struct sockaddr *)&from,
                                 (struct sockaddr *)&m_local, 0, 0,
                                 picoquic_current_time());
    }
    pump();
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
        ssize_t sent = sendto(m_fd, buf, send_len, 0, (struct sockaddr *)&to,
               to.ss_family == AF_INET ? sizeof(struct sockaddr_in)
                                       : sizeof(struct sockaddr_in6));
        if (qdbg()) fprintf(stderr, "quic[%p] tx %zu -> %zd (fam %d)\n",
                            (void *)this, send_len, sent, to.ss_family);
    }
    int64_t delay_us = picoquic_get_next_wake_delay(m_quic, picoquic_current_time(),
                                                    60ll * 1000000);
    m_loop.reset_timer(m_timer, (int)(delay_us / 1000) + 1);
}

} // namespace rivt::net
