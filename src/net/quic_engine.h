#pragma once
#include "core/event_loop.h"
#include "net/stun.h"
#include <cstdint>
#include <functional>
#include <map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

typedef struct st_picoquic_quic_t picoquic_quic_t;
typedef struct st_picoquic_cnx_t picoquic_cnx_t;

namespace rivt::net { class TurnRelay; }

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
        // Outbound bytes per stream that the stack hasn't pulled yet
        // (active-stream API: picoquic asks per stream when it can
        // send). Control rides stream 0; each pane gets its own stream
        // so one pane's bulk output can't head-of-line-block input or
        // other panes.
        struct StreamBuf {
            std::string out;
            size_t out_off = 0;
            size_t queued() const { return out.size() - out_off; }
        };
        std::map<uint64_t, StreamBuf> streams;
        size_t queued() const {  // aggregate, for connection backpressure
            size_t n = 0;
            for (const auto &[id, sb] : streams) n += sb.queued();
            return n;
        }
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

    // Route this connection's traffic through a TURN relay (listener
    // side). Inbound Data indications become QUIC packets; outbound
    // QUIC to the relayed peer goes out as Send indications. The peer
    // is identified by the address TURN reports (its reflexive).
    // Relays: a peer learned via a relay's Data indications is routed
    // back through that same relay. Several relays coexist (rivtd
    // rotates allocations; old ones keep serving established sessions).
    void add_turn(TurnRelay *turn);
    void remove_turn(TurnRelay *turn);
    ~QuicEngine();

    std::function<void(Conn *)> on_connected;   // handshake complete
    std::function<void(Conn *)> on_closed;      // any teardown, fires once
    std::function<void(Conn *, uint64_t stream, const uint8_t *, size_t)> on_data;
    // Send queue fell below the low-water mark: resume paused producers.
    std::function<void(Conn *)> on_drained;

    static constexpr size_t SEND_HIGH_WATER = 3 << 20;
    static constexpr size_t SEND_LOW_WATER = 512 << 10;

    void send(Conn *c, uint64_t stream, const void *data, size_t len);
    void close_conn(Conn *c);
    Conn *client_conn() { return m_client_conn; }  // client mode only
    // Seconds since any datagram last arrived on this engine (keepalives
    // included) — 0 if nothing yet. Basis for liveness/staleness.
    double seconds_since_rx() const;

    // True if a connection closed due to peer-certificate rejection
    // (TLS certificate_unknown) — i.e. we reached the peer but neither
    // side trusts the other (not in the same device set).
    bool cert_rejected() const { return m_cert_rejected; }

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

    bool m_cert_rejected = false;
    std::string m_label;  // "host:port" of an outbound target, for logs
    uint64_t m_last_rx_us = 0;  // picoquic_current_time() of last datagram
    std::vector<std::pair<struct sockaddr_in, TurnRelay *>> m_relayed_peers;
    void feed_relayed(TurnRelay *turn, const struct sockaddr_in &peer,
                      const uint8_t *d, size_t n);
    TurnRelay *relay_for(const struct sockaddr_storage &to, struct sockaddr_in *peer) const;

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
