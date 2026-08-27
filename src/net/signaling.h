#pragma once
#include "core/event_loop.h"
#include "net/rendezvous.h"   // net::Candidate
#include "net/ws_client.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rivt::net {

class Identity;

// Candidate exchange over the rendezvous WebSocket. Each device is
// addressed by its signaling id = SHA-256(SPKI) hex; the DO ferries an
// opaque payload between them. Used to trade STUN/local/TURN candidates
// for a NAT hole punch. Not part of the E2E trust (QUIC still pins
// certs), so the payload is unauthenticated relay data.
class Signaling {
public:
    Signaling(EventLoop &loop, const Identity &self);

    // Process-wide shared instance. The rendezvous DO addresses inbound
    // frames to our signaling id and delivers each only to the first
    // socket under that tag, so every peer must share one socket: a second
    // socket with the same id shadows the first and its answers are
    // misdelivered (a concurrent second connection would silently never
    // get its punch answer). Created and connected on first use, then kept
    // alive with a keepalive timer for the life of the process.
    static Signaling *shared(EventLoop &loop, const Identity &self,
                             const std::string &rendezvous_url);

    // wss connect to <rendezvous>/ws, tagged with our signaling id.
    bool start(const std::string &rendezvous_url);
    bool ready() const { return m_ws.is_open(); }
    const std::string &my_id() const { return m_id; }

    // Send our candidate set to peer `to_id` as an offer or answer.
    void send(const std::string &to_id, bool answer, const std::vector<Candidate> &cands);
    void keepalive();  // edge-answered ping to hold the NAT/TCP mapping

    // Route candidates from peer_id to cb until unsubscribe(peer_id).
    // Replaces any prior handler for that peer. Used by the client, which
    // multiplexes several peers over the one shared socket.
    void subscribe(const std::string &peer_id,
                   std::function<void(bool answer, std::vector<Candidate>)> cb);
    void unsubscribe(const std::string &peer_id);

    std::function<void()> on_ready;
    // Catch-all for peers with no subscription (the daemon answers offers
    // from anyone). from_id = peer's signaling id; answer = false = offer.
    std::function<void(const std::string &from_id, bool answer,
                       std::vector<Candidate>)> on_candidates;

private:
    void handle(const std::string &frame);

    EventLoop &m_loop;
    WsClient m_ws;
    std::string m_id;
    std::vector<std::string> m_pending;  // frames queued until the WS opens
    std::map<std::string, std::function<void(bool, std::vector<Candidate>)>> m_subs;
};

// SHA-256(data) as lowercase hex.
std::string sha256_hex(const std::string &data);

} // namespace rivt::net
