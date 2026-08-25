#pragma once
#include "core/event_loop.h"
#include <functional>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace rivt::net {

// Non-blocking WSS (WebSocket-over-TLS) client on the EventLoop. Used
// for the rendezvous signaling channel: a long-lived connection to the
// DO carrying presence and candidate exchange. Text frames only, plus
// automatic pong; server->client frames are never masked, ours are.
class WsClient {
public:
    explicit WsClient(EventLoop &loop) : m_loop(loop) {}
    ~WsClient();

    // wss://host[:port]/path. Resolves, TLS-connects, and upgrades, all
    // non-blocking; on_open fires when the handshake completes.
    bool connect(const std::string &url);
    void close();
    bool is_open() const { return m_state == State::Open; }

    void send_text(const std::string &msg);

    std::function<void()> on_open;
    std::function<void(const std::string &)> on_message;  // one text frame
    std::function<void()> on_close;  // transport gone (fires once)

private:
    enum class State { Idle, TcpConnect, TlsHandshake, Upgrade, Open, Dead };
    void on_event(uint32_t events);
    void pump_tls();          // drive SSL_connect / flush
    void do_upgrade_read();   // read HTTP 101 response
    void parse_frames();
    void encode_frame(int opcode, const std::string &payload);
    void want(bool rd, bool wr);
    void fail();

    EventLoop &m_loop;
    State m_state = State::Idle;
    int m_fd = -1;
    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;
    std::string m_host, m_path, m_ws_key;
    std::string m_out;      // plaintext pending SSL_write
    std::string m_in;       // decrypted bytes awaiting frame parse
    std::string m_http;     // upgrade response accumulator
    bool m_armed_write = false;
};

} // namespace rivt::net
