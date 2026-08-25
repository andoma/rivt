#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>

namespace rivt::net {

// Minimal STUN (RFC 5389) Binding, just enough for reflexive-address
// discovery and to demux STUN from QUIC on a shared UDP socket.

constexpr size_t STUN_TXID_LEN = 12;

// A datagram is STUN (vs QUIC) iff its two high bits are 0 and it
// carries the magic cookie. QUIC always sets the 0x40 fixed bit, so
// there is no overlap. RFC 7983 style.
bool is_stun(const uint8_t *data, size_t len);

// Build a Binding Request into out (>= 20 bytes); fills txid[12].
// Returns the message length (20).
size_t stun_build_request(uint8_t *out, uint8_t txid[STUN_TXID_LEN]);

// Parse a Binding Success Response. On success fills *mapped with the
// XOR-MAPPED-ADDRESS and returns true; verifies the txid matches.
bool stun_parse_response(const uint8_t *data, size_t len,
                         const uint8_t txid[STUN_TXID_LEN],
                         struct sockaddr_storage *mapped);

} // namespace rivt::net
