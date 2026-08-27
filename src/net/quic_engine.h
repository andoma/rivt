#pragma once
#include "core/event_loop.h"
#include "net/stun.h"
#include <cstdint>
#include <functional>
#include <sys/socket.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

typedef struct st_picoquic_quic_t picoquic_quic_t;
typedef struct st_picoquic_cnx_t picoquic_cnx_t;

namespace rivt::net {

class Identity;

// Epoll-embedded picoquic engine. One engine owns one UDP socket and a
// QUIC context, in server (listen) or client (connect) mode. The rivt
// framing rides a single bidirectional stream per connection (stream 0)
// — same head-of-line behavior as the unix socket it replaces; per-pane
// stream fan-out is a later refinement.
class QuicEngine {
public:
    struct Conn {
        picoquic_cnx_t *cnx = nullptr;
        QuicEngine *engine = nullptr;
        bool established = false;
        bool dead = false;
        void *user = nullptr;  // owner's per-connection state
        // Outbound bytes the stack hasn't pulled yet (active-stream
        // API: picoquic asks for data when it can actually send).
        std::string out;
        size_t out_off = 0;
        size_t queued() const { return out.size() - out_off; }
    };

    // Mutual authentication in both modes: our cert/key from identity,
    // peers validated against the authorized-certs bundle.
    static std::unique_ptr<QuicEngine> listen(EventLoop &loop, uint16_t port,
                                              const Identity &id,
                                              const std::string &authorized_bundle);
    static std::unique_ptr<QuicEngine> connect(EventLoop &loop, const std::string &host,
                                               uint16_t port, const Identity &id,
                                               const std::string &authorized_bundle);

    // Split form for NAT traversal: create the client engine (binds the
    // socket, so it can be STUNed) without dialing, then start the
    // connection to a peer address learned via signaling — from the
    // same socket whose reflexive address was advertised.
    static std::unique_ptr<QuicEngine> create_client(EventLoop &loop, const Identity &id,
                                                     const std::string &authorized_bundle);
    bool start_connection(const std::string &host, uint16_t port);

    // Send a few small datagrams to addr from the live socket to open a
    // NAT mapping toward the peer (hole punch). Harmless if it arrives:
    // not valid QUIC, so the peer's stack ignores it.
    void punch(const std::string &host, uint16_t port);
    ~QuicEngine();

    std::function<void(Conn *)> on_connected;   // handshake complete
    std::function<void(Conn *)> on_closed;      // any teardown, fires once
    std::function<void(Conn *, const uint8_t *, size_t)> on_data;
    // Send queue fell below the low-water mark: resume paused producers.
    std::function<void(Conn *)> on_drained;

    static constexpr size_t SEND_HIGH_WATER = 3 << 20;
    static constexpr size_t SEND_LOW_WATER = 512 << 10;

    void send(Conn *c, const void *data, size_t len);
    void close_conn(Conn *c);
    Conn *client_conn() { return m_client_conn; }  // client mode only

    // Discover this socket's reflexive (public) transport address via a
    // STUN server (default stun.cloudflare.com:3478). The callback fires
    // once with the mapped address, or with ok=false on timeout. STUN
    // shares the QUIC socket and is demuxed by first byte.
    void discover_reflexive(std::function<void(bool ok, const struct sockaddr_storage &)> cb,
                            const char *stun_host = nullptr, uint16_t stun_port = 3478);
    // The bound local UDP port (for candidate advertisement).
    uint16_t local_port() const;

    // Internal, used by the C callback trampoline in quic_engine.cpp.
    int handle_event(picoquic_cnx_t *cnx, Conn *conn, uint64_t stream_id,
                     uint8_t *bytes, size_t length, int event);
    Conn *adopt(picoquic_cnx_t *cnx);

private:
    QuicEngine(EventLoop &loop) : m_loop(loop) {}
    bool init(uint16_t bind_port, const Identity &id, const std::string &bundle,
              bool server);
    void mark_closed(Conn *c);
    void on_socket(uint32_t events);
    void pump();

    EventLoop &m_loop;
    picoquic_quic_t *m_quic = nullptr;
    int m_fd = -1;
    int m_timer = -1;
    struct sockaddr_storage m_local {};
    bool m_want_write = false;

    // Pending STUN reflexive query (at most one at a time).
    bool m_stun_active = false;
    uint8_t m_stun_txid[STUN_TXID_LEN] = {0};
    struct sockaddr_storage m_stun_server {};
    int m_stun_timer = -1;
    int m_stun_tries = 0;
    std::function<void(bool, const struct sockaddr_storage &)> m_stun_cb;
    void stun_send();
    std::vector<std::unique_ptr<Conn>> m_conns;
    Conn *m_client_conn = nullptr;
};

} // namespace rivt::net
