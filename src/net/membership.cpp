#include "net/membership.h"
#include "core/debug.h"
#include "net/identity.h"
#include "net/rendezvous.h"
#include "proto/wire.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace rivt::net {

enum OpType : uint8_t { OP_GENESIS = 1, OP_ADD = 2, OP_REMOVE = 3 };

struct ParsedOp {
    uint8_t type = 0;
    uint32_t seq = 0;
    std::string prev;      // 32 bytes
    std::string subject;   // pubkey DER
    std::string name;
    std::string cert;
    int64_t expires = 0;
    std::string signer;    // pubkey DER
    std::string sig;       // DER ECDSA
    size_t body_len = 0;   // bytes covered by the signature
};

// Body = everything the signature covers (all fields except sig).
static void write_body(proto::Writer &w, uint8_t type, uint32_t seq,
                       const std::string &prev, const std::string &subject,
                       const std::string &name, const std::string &cert,
                       int64_t expires, const std::string &signer) {
    w.u8(type);
    w.u32(seq);
    w.bytes(prev.data(), prev.size());  // always 32
    w.str(subject);
    w.str(name);
    w.str(cert);
    w.u64((uint64_t)expires);
    w.str(signer);
}

static bool parse_op(const std::string &op, ParsedOp &out) {
    proto::Reader r((const uint8_t *)op.data(), op.size());
    out.type = r.u8();
    out.seq = r.u32();
    if (!r.ok || r.remaining() < 32) return false;
    out.prev.assign((const char *)r.p, 32);
    r.skip(32);
    out.subject = r.str();
    out.name = r.str();
    out.cert = r.str();
    out.expires = (int64_t)r.u64();
    out.signer = r.str();
    if (!r.ok) return false;
    out.body_len = op.size() - r.remaining() - 0;  // bytes consumed so far
    // body_len must equal bytes before the trailing sig field.
    out.body_len = (size_t)((const char *)r.p - op.data());
    out.sig = r.str();
    return r.ok;
}

static std::string sha256(const std::string &d) {
    uint8_t h[SHA256_DIGEST_LENGTH];
    SHA256((const uint8_t *)d.data(), d.size(), h);
    return std::string((const char *)h, sizeof h);
}

static std::string hex(const std::string &b) {
    static const char *t = "0123456789abcdef";
    std::string o;
    for (unsigned char c : b) { o += t[c >> 4]; o += t[c & 15]; }
    return o;
}

static EVP_PKEY *pub_from_der(const std::string &der) {
    const uint8_t *p = (const uint8_t *)der.data();
    return d2i_PUBKEY(nullptr, &p, (long)der.size());
}

static EVP_PKEY *priv_from_pem(const std::string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return nullptr;
    EVP_PKEY *k = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
    fclose(f);
    return k;
}

static std::string sign_der(EVP_PKEY *key, const std::string &body) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    std::string out;
    size_t len = 0;
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestSign(ctx, nullptr, &len, (const uint8_t *)body.data(), body.size()) == 1) {
        out.resize(len);
        if (EVP_DigestSign(ctx, (uint8_t *)out.data(), &len, (const uint8_t *)body.data(),
                           body.size()) == 1)
            out.resize(len);
        else
            out.clear();
    }
    EVP_MD_CTX_free(ctx);
    return out;
}

static bool verify_der(const std::string &pubkey_der, const std::string &body,
                       const std::string &sig) {
    EVP_PKEY *key = pub_from_der(pubkey_der);
    if (!key) return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
              EVP_DigestVerify(ctx, (const uint8_t *)sig.data(), sig.size(),
                               (const uint8_t *)body.data(), body.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

static std::string build_op(EVP_PKEY *signer_key, uint8_t type, uint32_t seq,
                            const std::string &prev, const std::string &subject,
                            const std::string &name, const std::string &cert,
                            int64_t expires, const std::string &signer_pub) {
    proto::Writer body;
    write_body(body, type, seq, prev, subject, name, cert, expires, signer_pub);
    std::string bstr((const char *)body.buf.data(), body.buf.size());
    std::string sig = sign_der(signer_key, bstr);
    if (sig.empty()) return {};
    proto::Writer full;
    write_body(full, type, seq, prev, subject, name, cert, expires, signer_pub);
    full.str(sig);
    return std::string((const char *)full.buf.data(), full.buf.size());
}

std::string MembershipLog::genesis(const Identity &founder) {
    EVP_PKEY *key = priv_from_pem(founder.key_path());
    if (!key) return {};
    std::string pub = founder.spki_der();
    std::string op = build_op(key, OP_GENESIS, 0, std::string(32, '\0'), pub,
                              founder.fingerprint(), founder.cert_pem(), 0, pub);
    EVP_PKEY_free(key);
    return op;
}

std::string MembershipLog::add_member(const Identity &signer, const std::string &pubkey_der,
                                      const std::string &name, const std::string &cert_pem,
                                      int64_t expires) {
    if (m_ops.empty()) return {};
    EVP_PKEY *key = priv_from_pem(signer.key_path());
    if (!key) return {};
    std::string prev = sha256(m_ops.back());
    std::string op = build_op(key, OP_ADD, (uint32_t)m_ops.size(), prev, pubkey_der, name,
                              cert_pem, expires, signer.spki_der());
    EVP_PKEY_free(key);
    if (op.empty() || !append_verified(op)) return {};
    return op;
}

std::string MembershipLog::remove_member(const Identity &signer, const std::string &pubkey_der) {
    if (m_ops.empty()) return {};
    EVP_PKEY *key = priv_from_pem(signer.key_path());
    if (!key) return {};
    std::string prev = sha256(m_ops.back());
    std::string op = build_op(key, OP_REMOVE, (uint32_t)m_ops.size(), prev, pubkey_der, "",
                              "", 0, signer.spki_der());
    EVP_PKEY_free(key);
    if (op.empty() || !append_verified(op)) return {};
    return op;
}

bool MembershipLog::append_verified(const std::string &op) {
    ParsedOp p;
    if (!parse_op(op, p)) return false;
    std::string body = op.substr(0, p.body_len);

    if (m_ops.empty()) {
        if (p.type != OP_GENESIS || p.seq != 0 || p.prev != std::string(32, '\0')) return false;
        if (p.signer != p.subject) return false;  // self-signed anchor
        if (!verify_der(p.subject, body, p.sig)) return false;
        m_ops.push_back(op);
        m_state.push_back({p.subject, p.name, p.cert, p.expires});
        return true;
    }

    if (p.seq != m_ops.size()) return false;
    if (p.prev != sha256(m_ops.back())) return false;
    // Signer must be a member per ops so far (expiry not applied to
    // historical authority — add/remove drive it).
    auto it = std::find_if(m_state.begin(), m_state.end(),
                           [&](const Member &m) { return m.pubkey_der == p.signer; });
    if (it == m_state.end()) return false;
    if (!verify_der(p.signer, body, p.sig)) return false;

    if (p.type == OP_ADD) {
        auto ex = std::find_if(m_state.begin(), m_state.end(),
                               [&](const Member &m) { return m.pubkey_der == p.subject; });
        if (ex != m_state.end())
            *ex = {p.subject, p.name, p.cert, p.expires};
        else
            m_state.push_back({p.subject, p.name, p.cert, p.expires});
    } else if (p.type == OP_REMOVE) {
        m_state.erase(std::remove_if(m_state.begin(), m_state.end(),
                                     [&](const Member &m) { return m.pubkey_der == p.subject; }),
                      m_state.end());
    } else {
        return false;
    }
    m_ops.push_back(op);
    return true;
}

bool MembershipLog::load(const std::vector<std::string> &ops) {
    m_ops.clear();
    m_state.clear();
    for (const auto &op : ops) {
        if (!append_verified(op)) {
            m_ops.clear();
            m_state.clear();
            return false;
        }
    }
    return true;
}

std::vector<Member> MembershipLog::members(int64_t now) const {
    std::vector<Member> out;
    for (const auto &m : m_state)
        if (now <= 0 || m.expires == 0 || m.expires > now) out.push_back(m);
    return out;
}

bool MembershipLog::is_member(const std::string &pubkey_der, int64_t now) const {
    for (const auto &m : members(now))
        if (m.pubkey_der == pubkey_der) return true;
    return false;
}

std::string MembershipLog::set_id() const {
    if (m_ops.empty()) return {};
    return hex(sha256(m_ops.front()));
}

std::string MembershipLog::default_path() {
    const char *st = getenv("RIVT_STATE_DIR");
    if (st && *st) { std::string d = st; mkdir(d.c_str(), 0700); return d + "/membership.log"; }
    const char *xdg = getenv("XDG_STATE_HOME");
    std::string d = xdg && *xdg ? std::string(xdg) + "/rivt"
                                : std::string(getenv("HOME") ? getenv("HOME") : ".") +
                                      "/.local/state/rivt";
    mkdir(d.c_str(), 0700);
    return d + "/membership.log";
}

bool MembershipLog::save(const std::string &path) const {
    proto::Writer w;
    w.u32((uint32_t)m_ops.size());
    for (const auto &op : m_ops) w.str(op);
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(w.buf.data(), 1, w.buf.size(), f) == w.buf.size();
    fclose(f);
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool MembershipLog::load_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, n);
    fclose(f);
    proto::Reader r((const uint8_t *)data.data(), data.size());
    uint32_t cnt = r.u32();
    std::vector<std::string> ops;
    for (uint32_t i = 0; i < cnt && r.ok; i++) ops.push_back(r.str());
    if (!r.ok) return false;
    return load(ops);
}

bool MembershipLog::write_bundle(const std::string &bundle_path, int64_t now) const {
    std::string tmp = bundle_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    chmod(tmp.c_str(), 0600);
    for (const auto &m : members(now))
        fwrite(m.cert_pem.data(), 1, m.cert_pem.size(), f);
    fclose(f);
    return rename(tmp.c_str(), bundle_path.c_str()) == 0;
}

std::string sync_membership(const Identity &self, bool found_if_missing) {
    std::string path = MembershipLog::default_path();
    MembershipLog log;
    if (!log.load_file(path)) {
        if (!found_if_missing) {
            rivt::logmsg("rivt: this device is not a member of any set "
                            "(join one with `rivt join <code>`)\n");
            return {};
        }
        std::string g = MembershipLog::genesis(self);
        if (g.empty() || !log.load({g})) {
            rivt::logmsg("rivt: failed to create device set\n");
            return {};
        }
        log.save(path);
        rivt::logmsg("rivt: founded device set %s\n", log.set_id().c_str());
    }

    std::string rdv = rendezvous_url();
    if (!rdv.empty()) {
        std::string set = log.set_id();
        std::vector<std::string> remote;
        if (membership_fetch(rdv, set, remote) && remote.size() > log.size()) {
            MembershipLog cand;
            if (cand.load(remote) && cand.set_id() == set) {
                log = std::move(cand);
                log.save(path);
            }
        }
        std::vector<std::string> have;
        membership_fetch(rdv, set, have);
        for (size_t i = have.size(); i < log.size(); i++)
            membership_push(rdv, set, (uint32_t)i, log.ops()[i]);
    }

    if (!log.write_bundle(Identity::authorized_bundle_path(), (int64_t)time(nullptr)))
        rivt::logmsg("rivt: failed to write trust bundle\n");
    return log.set_id();
}

} // namespace rivt::net
