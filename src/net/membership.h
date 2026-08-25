#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rivt::net {

class Identity;

// The device set as a signed, append-only operation log. Every op is
// signed by a device that is a member at that point; op 0 (genesis) is
// self-signed and is the trust anchor. The log is verified in full by
// every client, so the storage/distribution layer (the DO) can neither
// forge nor tamper — only withhold. Members derive by folding the log.

struct Member {
    std::string pubkey_der;  // SPKI DER (the identity)
    std::string name;        // display label
    std::string cert_pem;    // self-signed QUIC cert (its own root)
    int64_t expires = 0;     // unix seconds; 0 = never
};

class MembershipLog {
public:
    // --- build ops (return the serialized op, "" on failure) ---
    // Found a new set: genesis op, self-signed by `founder`.
    static std::string genesis(const Identity &founder);

    // Append an add/remove signed by `signer` (must be a current member
    // of *this* log). Adds the op to the log on success.
    std::string add_member(const Identity &signer, const std::string &pubkey_der,
                           const std::string &name, const std::string &cert_pem,
                           int64_t expires = 0);
    std::string remove_member(const Identity &signer, const std::string &pubkey_der);

    // --- load / verify ---
    // Replace the log with `ops`, verifying the whole chain (genesis,
    // seq/prev linkage, each signer a member at that point, signatures).
    // Returns false and leaves the log empty on any violation.
    bool load(const std::vector<std::string> &ops);

    // Current members (expired entries excluded when now > 0).
    std::vector<Member> members(int64_t now = 0) const;
    bool is_member(const std::string &pubkey_der, int64_t now = 0) const;

    const std::vector<std::string> &ops() const { return m_ops; }
    size_t size() const { return m_ops.size(); }

private:
    bool append_verified(const std::string &op);

    std::vector<std::string> m_ops;
    // Running membership state after the last verified op (pre-expiry).
    std::vector<Member> m_state;
};

} // namespace rivt::net
