#pragma once
#include "core/event_loop.h"
#include <cstdint>
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
    };

    // Mutual authentication in both modes: our cert/key from identity,
    // peers validated against the authorized-certs bundle.
    static std::unique_ptr<QuicEngine> listen(EventLoop &loop, uint16_t port,
                                              const Identity &id,
                                              const std::string &authorized_bundle);
    static std::unique_ptr<QuicEngine> connect(EventLoop &loop, const std::string &host,
                                               uint16_t port, const Identity &id,
                                               const std::string &authorized_bundle);
    ~QuicEngine();

    std::function<void(Conn *)> on_connected;   // handshake complete
    std::function<void(Conn *)> on_closed;      // any teardown, fires once
    std::function<void(Conn *, const uint8_t *, size_t)> on_data;

    void send(Conn *c, const void *data, size_t len);
    void close_conn(Conn *c);
    Conn *client_conn() { return m_client_conn; }  // client mode only

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
    std::vector<std::unique_ptr<Conn>> m_conns;
    Conn *m_client_conn = nullptr;
};

} // namespace rivt::net
