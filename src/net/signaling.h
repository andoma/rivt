#pragma once
#include "core/event_loop.h"
#include "net/rendezvous.h"   // net::Candidate
#include "net/ws_client.h"
#include <functional>
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

    // wss connect to <rendezvous>/ws, tagged with our signaling id.
    bool start(const std::string &rendezvous_url);
    bool ready() const { return m_ws.is_open(); }
    const std::string &my_id() const { return m_id; }

    // Send our candidate set to peer `to_id` as an offer or answer.
    void send(const std::string &to_id, bool answer, const std::vector<Candidate> &cands);

    std::function<void()> on_ready;
    // from_id = peer's signaling id; answer = false for offer.
    std::function<void(const std::string &from_id, bool answer,
                       std::vector<Candidate>)> on_candidates;

private:
    void handle(const std::string &frame);

    EventLoop &m_loop;
    WsClient m_ws;
    std::string m_id;
};

// SHA-256(data) as lowercase hex.
std::string sha256_hex(const std::string &data);

} // namespace rivt::net
