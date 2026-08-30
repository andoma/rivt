#pragma once
#include "core/event_loop.h"
#include "net/identity.h"
#include "net/quic_engine.h"
#include "net/rendezvous.h"
#include "net/signaling.h"
#include "proto/wire.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
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

    // Wake / network-change hooks. suspend_probes() parks the link
    // watchdog (lid closed: don't declare anything dead while frozen);
    // verify_link() sends a Ping so the ACK probe either confirms the
    // path within seconds or triggers a reconnect — without waiting for
    // the user to type.
    void suspend_probes() { disarm_ack_probe(); }
    void verify_link();
    void list_sessions();
    void create_session(const std::string &name, const std::string &cwd, int cols, int rows);
    // Attach; when we hold anchored output offsets for the session's
    // panes (from a previous attach in this daemon epoch), they ride
    // along so the daemon can resume streams instead of snapshotting.
    void attach(uint32_t session_id);
    void detach();
    void resize_session(int cols, int rows);
    void split(uint32_t pane_id, bool horizontal);  // false = side by side
    void close_pane(uint32_t pane_id);
    void resize_pane(uint32_t pane_id, int dx_cells, int dy_cells);
    void new_window();
    void close_window(uint32_t window_id);
    void fetch_scrollback(uint32_t pane_id, uint32_t end_abs, uint32_t count);
    void kill_session(uint32_t session_id);
    // Sends input tagged with a per-pane sequence number; returns it.
    // PANE_ACK confirms the daemon's output reflects input through seq.
    uint32_t send_input(uint32_t pane_id, const char *data, size_t len);

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
    std::function<void(uint32_t pane_id, uint32_t seq, bool echo_off)> on_pane_ack;
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
    // Smoothed transport RTT in ms (0 = unknown / not QUIC).
    int rtt_ms() const;

private:
    void on_event(uint32_t ev);
    void process();
    void process_buffer(std::string &in, uint64_t quic_stream);
    void dispatch_control(uint16_t type, const uint8_t *data, size_t len);
    void send_frame(uint16_t channel, uint16_t type, const void *data, size_t len);
    void send_control(uint16_t type, const proto::Writer &w) {
        send_frame(0, type, w.buf.data(), w.buf.size());
    }
    void flush();
    void fail();
    void arm_ack_probe();     // watch for any reply after a QUIC send
    void disarm_ack_probe();

    // SSH agent forwarding: daemon-side connections to the session's
    // SSH_AUTH_SOCK bridged to our local agent, one fd per stream id.
    struct AgentBridge {
        int fd = -1;
        std::string out;
        size_t out_off = 0;
        bool write_armed = false;
    };
    std::unordered_map<uint32_t, AgentBridge> m_agent;
    void agent_open(uint32_t id);
    void agent_event(uint32_t id, uint32_t ev);
    void agent_flush(AgentBridge &b, uint32_t id);
    void agent_close(uint32_t id, bool notify);
    void agent_close_all();

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
    void set_link(const std::string &st);
    std::vector<std::unique_ptr<net::QuicEngine>> m_stale_probes;  // parked, freed on fresh stack
    std::string m_pending_out;  // frames sent before a probe won

    // NAT traversal: exchange candidates and punch / relay. The signaling
    // socket is process-wide shared (see Signaling::shared); we only borrow
    // it and unsubscribe our peer on teardown, never own it.
    net::Signaling *m_signaling = nullptr;
    std::string m_sig_peer;  // peer id we subscribed, for unsubscribe
    int m_turn_fallback_timer = -1;
    int m_offer_retry_timer = -1;
    int m_ack_probe_timer = -1;
    std::chrono::steady_clock::time_point m_await_since{};
    void begin_punch(const RemoteEndpoint &ep);
    void on_answer(const std::vector<net::Candidate> &server_cands);
    // Punched-path targets, kept for re-dials within the punch window
    // (a handshake that spans a link outage dies; the next one lands).
    net::Candidate m_punch_stun {};
    net::Candidate m_punch_turn {};
    void adopt_probe(size_t idx);
    void probe_failed();
    // close() can run inside m_quic's own callback (on_closed -> fail);
    // the engine is parked here and disposed on a fresh stack (next
    // connect, or our destructor).
    std::unique_ptr<net::QuicEngine> m_stale_quic;
    net::QuicEngine::Conn *m_quic_conn = nullptr;
    int m_fd = -1;
    std::string m_in, m_out;
    std::unordered_map<uint64_t, std::string> m_qin;      // per-QUIC-stream reassembly
    std::unordered_map<uint16_t, uint64_t> m_pane_stream;  // learned pane -> stream
    std::unordered_map<uint32_t, uint32_t> m_in_seq;        // per-pane input seq
    // Seamless re-attach: absolute output offset our replica is current
    // through, per pane — anchored by PANE_RESUME, advanced by PANE_OUT.
    // Survives close() deliberately; invalidated on daemon-epoch change.
    std::unordered_map<uint32_t, uint64_t> m_pane_off;
    uint32_t m_daemon_epoch = 0;
    size_t m_out_off = 0;
    bool m_write_armed = false;
    bool m_failing = false;
};

} // namespace rivt
