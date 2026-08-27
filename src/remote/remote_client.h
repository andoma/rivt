#pragma once
#include "core/event_loop.h"
#include "net/identity.h"
#include "net/quic_engine.h"
#include "net/rendezvous.h"
#include "net/signaling.h"
#include "proto/wire.h"
#include <cstdint>
#include <functional>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace rivt {

// Where a daemon lives: a local unix socket, or typed QUIC candidates
// (LAN / observed / stun / turn — see net::Candidate).
struct RemoteEndpoint {
    std::string unix_path;
    std::vector<net::Candidate> candidates;
    std::string peer_sig_id;    // for NAT-traversal signaling (optional)
    std::string rendezvous;     // rendezvous URL (optional)
    bool is_quic() const { return !candidates.empty(); }
};

struct RemoteSessionInfo {
    uint32_t id;
    std::string name;
    uint32_t npanes;
};

struct RemotePaneGeom {
    uint32_t id;
    int x, y, cols, rows;  // cell units within the session grid
};

// Client side of the rivtd attach protocol: framing, command encoding,
// event dispatch. Sibling of TmuxClient. Owns no panes — that's
// RemoteController's job.
class RemoteClient {
public:
    explicit RemoteClient(EventLoop &loop) : m_loop(loop) {}
    ~RemoteClient() { close(); }

    static std::string default_socket_path();

    // One-shot blocking roster query (used before any window exists).
    // Spawns the daemon if autostart and nobody is listening.
    static bool query_sessions(const std::string &path, bool autostart,
                               std::vector<RemoteSessionInfo> &out);

    // Connect to rivtd; if autostart, spawn one (double-forked, detached)
    // when nobody is listening and retry briefly. (Unix transport only;
    // QUIC daemons are started on their own machine.)
    bool connect(const std::string &path, bool autostart);
    bool connect(const RemoteEndpoint &ep, bool autostart);
    // Redo the last connect (daemon upgrade/restart, roaming).
    bool reconnect() { return connect(m_endpoint, false); }
    void close();
    bool connected() const { return m_fd >= 0 || m_quic_conn != nullptr; }
    EventLoop &loop() { return m_loop; }

    // Commands
    void hello();
    void list_sessions();
    void create_session(const std::string &name, const std::string &cwd, int cols, int rows);
    void attach(uint32_t session_id);
    void detach();
    void resize_session(int cols, int rows);
    void split(uint32_t pane_id, bool horizontal);  // false = side by side
    void close_pane(uint32_t pane_id);
    void new_window();
    void close_window(uint32_t window_id);
    void fetch_scrollback(uint32_t pane_id, uint32_t end_abs, uint32_t count);
    void kill_session(uint32_t session_id);
    void send_input(uint32_t pane_id, const char *data, size_t len);

    // Events. on_disconnect may be invoked from inside the read
    // callback; receivers must defer destruction of this object
    // (same rule as TmuxClient).
    std::function<void()> on_hello_ok;
    std::function<void(const std::vector<RemoteSessionInfo> &)> on_session_list;
    std::function<void(uint32_t sid, uint32_t pane_id, const std::string &error)> on_session_created;
    std::function<void(uint32_t sid)> on_attach_ok;
    std::function<void(uint32_t sid, uint32_t wid)> on_window_added;
    std::function<void(uint32_t sid, uint32_t wid)> on_window_closed;
    std::function<void(uint32_t sid, uint32_t wid, int cols, int rows,
                       const std::vector<RemotePaneGeom> &)> on_layout;
    std::function<void(uint32_t pane_id, const uint8_t *, size_t)> on_snapshot;
    std::function<void(uint32_t pane_id, const uint8_t *, size_t)> on_scrollback;
    std::function<void(uint32_t pane_id, const char *, size_t)> on_output;
    std::function<void(uint32_t pane_id)> on_pane_exited;
    std::function<void(uint32_t sid)> on_session_closed;
    std::function<void(const std::string &)> on_error;
    std::function<void()> on_disconnect;
    // Fired when the connection status changes (adopted, reconnecting).
    std::function<void()> on_status;

    // How we connected, once adopted: "direct", "relay", or "lan".
    const std::string &transport() const { return m_transport; }
    // "connecting" | "connected" | "reconnecting".
    const std::string &link_state() const { return m_link_state; }
    // Seconds since we last received any bytes (for staleness).
    double seconds_since_rx() const;

private:
    void on_event(uint32_t ev);
    void process();
    void dispatch_control(uint16_t type, const uint8_t *data, size_t len);
    void send_frame(uint16_t channel, uint16_t type, const void *data, size_t len);
    void send_control(uint16_t type, const proto::Writer &w) {
        send_frame(0, type, w.buf.data(), w.buf.size());
    }
    void flush();
    void fail();

    EventLoop &m_loop;
    RemoteEndpoint m_endpoint;
    std::unique_ptr<net::Identity> m_identity;
    std::unique_ptr<net::QuicEngine> m_quic;
    // Happy-eyeballs candidate probing: all candidates race, the first
    // established handshake is adopted, the rest are parked.
    std::vector<std::unique_ptr<net::QuicEngine>> m_probes;
    std::vector<std::string> m_probe_kinds;   // parallel: candidate kind per probe
    std::string m_transport;                  // winning transport (display)
    std::string m_link_state = "connecting";
    int64_t m_last_rx_ms = 0;
    void note_rx();
    void set_link(const std::string &st);
    std::vector<std::unique_ptr<net::QuicEngine>> m_stale_probes;  // parked, freed on fresh stack
    std::string m_pending_out;  // frames sent before a probe won

    // NAT traversal: exchange candidates and punch / relay.
    std::unique_ptr<net::Signaling> m_signaling;
    int m_turn_fallback_timer = -1;
    void begin_punch(const RemoteEndpoint &ep);
    void on_answer(const std::vector<net::Candidate> &server_cands);
    void adopt_probe(size_t idx);
    void probe_failed();
    // close() can run inside m_quic's own callback (on_closed -> fail);
    // the engine is parked here and disposed on a fresh stack (next
    // connect, or our destructor).
    std::unique_ptr<net::QuicEngine> m_stale_quic;
    net::QuicEngine::Conn *m_quic_conn = nullptr;
    int m_fd = -1;
    std::string m_in, m_out;
    size_t m_out_off = 0;
    bool m_write_armed = false;
    bool m_failing = false;
};

} // namespace rivt
