#pragma once
#include <string>

namespace rivt::net {

class Identity;

// Device pairing over the rendezvous mailbox. The 128-bit secret in the
// invite code (transported out-of-band by the human) mutually
// authenticates the exchange; the DO only relays, so it can neither
// join the set nor forge either side. Both calls block (foreground CLI)
// and return true on success.

// Existing member: found a set if solo, mint an invite, print the code,
// wait for a join, verify it, prompt for approval (RIVT_PAIR_YES=1 auto-
// confirms), sign the add-op, and hand back the log.
bool pair_invite(const Identity &self);

// New device: redeem a code — send our key, wait for approval, verify
// the returned log (HMAC + chain + our own membership), and adopt it.
bool pair_join(const std::string &code, const Identity &self);

// Shared enrollment core for `rivt join` / `rivtd join`: if code is
// non-empty, join with it; if empty, prompt (paste a code to join, or
// Enter to found a new set). Writes config + trust bundle. Returns true
// on success. The daemon adds its own systemd step afterward.
bool interactive_enroll(const std::string &code_arg);

} // namespace rivt::net
