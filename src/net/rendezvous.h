#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rivt::net {

class Identity;

// Device directory client (phase 3). The rendezvous binds names to
// device keys and stores connection candidates. It is NOT part of the
// E2E trust: peers pin each other's certificates, so a hostile
// directory can at worst deny service.

// Base URL from $RIVT_RENDEZVOUS or ~/.config/rivt/rendezvous (single
// line). Empty string when unconfigured.
std::string rendezvous_url();

// Persist the rendezvous URL to ~/.config/rivt/rendezvous (mkdir -p).
bool set_rendezvous_url(const std::string &url);

struct DirEntry {
    std::string fingerprint;
    uint16_t port = 0;
    std::vector<std::string> addrs;  // local candidates, then observed ip
    int64_t last_seen_ms = 0;
};

// Blocking HTTPS calls — run from a forked child (daemon registration)
// or before any window exists (client lookup).
bool register_device(const std::string &base_url, const Identity &id,
                     const std::string &name, uint16_t port);
bool lookup_device(const std::string &base_url, const std::string &name, DirEntry &out);

// Non-loopback interface addresses, v4 first.
std::vector<std::string> local_addresses();

// --- membership log sync (ops are opaque binary; base64 on the wire) ---
// Push one op at expected position seq. Returns: 0 = appended,
// 1 = seq conflict (caller should re-fetch and retry), -1 = error.
int membership_push(const std::string &base_url, const std::string &set_id,
                    uint32_t seq, const std::string &op);
// Fetch all ops for a set (verified by the caller, not here).
bool membership_fetch(const std::string &base_url, const std::string &set_id,
                      std::vector<std::string> &ops_out);

// --- pairing mailbox (opaque base64 payloads, box = "offer"/"answer") ---
bool pair_put(const std::string &base_url, const std::string &invite_id,
              const std::string &box, const std::string &payload_b64);
// Returns true and fills out when a payload is present; false if empty/error.
bool pair_get(const std::string &base_url, const std::string &invite_id,
              const std::string &box, std::string &payload_b64_out);

// base64 (std, with padding) — shared with pairing.
std::string b64_encode(const std::string &in);
std::string b64_decode(const std::string &in);

} // namespace rivt::net
