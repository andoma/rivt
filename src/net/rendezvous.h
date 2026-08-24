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

} // namespace rivt::net
