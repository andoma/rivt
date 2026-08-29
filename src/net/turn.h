#pragma once
#include "core/event_loop.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>

namespace rivt::net {

// TURN relay allocation (RFC 8656 subset, IPv4) against Cloudflare.
// The *server* side of a rivt connection allocates a relay, permits the
// client's reflexive address, and advertises the relayed address as a
// [turn] candidate. The client then sends QUIC straight to that public
// address; this class unwraps inbound Data indications into QUIC
// packets and wraps outbound QUIC into Send indications. The client
// needs no TURN code — it just talks to a public address.
//
// Allocation is a short blocking handshake at setup; steady-state
// Send/Data is async on the EventLoop.
class TurnRelay {
public:
    explicit TurnRelay(EventLoop &loop) : m_loop(loop) {}
    ~TurnRelay();

    // Blocking: resolve, Allocate (long-term cred), start refresh timer.
    // Returns false on failure. On success relayed_host()/relayed_port()
    // hold the public relay address to advertise.
    bool allocate(const std::string &turn_host, uint16_t turn_port,
                  const std::string &user, const std::string &pass);

    const std::string &relayed_host() const { return m_relayed_host; }
    uint16_t relayed_port() const { return m_relayed_port; }

    // False once an allocation refresh has failed OR the data path has
    // gone silent despite self-probes: the relayed address is dead at
    // the TURN server, and the owner must allocate a fresh relay. Never
    // flips back to true.
    bool alive() const { return m_alive; }

    // The owner sends a probe through its own relayed address on a
    // steady cadence (peer traffic keeps Cloudflare's relayed port
    // warm — control-plane refreshes alone do not). Once armed,
    // refresh() treats prolonged Data-indication silence as a dead
    // relay. Control responses are no evidence: a relay was observed
    // answering refreshes for an hour while forwarding nothing.
    void expect_probes() { m_expect_probes = true; }
    bool probing() const { return m_expect_probes; }
    double seconds_since_data() const;

    // Allow a peer (its reflexive address) to reach our relay.
    void permit(const struct sockaddr_in &peer);

    // Outbound: wrap data to peer in a Send indication.
    void send_to(const struct sockaddr_in &peer, const uint8_t *data, size_t len);

    // Inbound Data indication payload, with the peer address it came from.
    std::function<void(const struct sockaddr_in &peer, const uint8_t *, size_t)> on_data;

private:
    void on_socket();
    void refresh();
    bool request(uint16_t type, bool with_auth, const struct sockaddr_in *peer,
                 uint8_t *resp, size_t *resp_len, int attempts = 3);
    void dispatch_data_indication(const uint8_t *buf, size_t n);

    EventLoop &m_loop;
    int m_fd = -1;
    int m_refresh_timer = -1;
    int m_keepalive_timer = -1;
    struct sockaddr_in m_server {};
    std::string m_user, m_pass, m_realm, m_nonce;
    uint8_t m_key[16] = {0};
    std::string m_relayed_host;
    uint16_t m_relayed_port = 0;
    bool m_alive = true;
    bool m_expect_probes = false;
    std::chrono::steady_clock::time_point m_last_data{};
    // Permitted peers, re-issued on every refresh: TURN permissions expire
    // after 300s, so a one-shot permit stalls a relay session at ~5 min.
    std::vector<struct sockaddr_in> m_peers;
};

} // namespace rivt::net
