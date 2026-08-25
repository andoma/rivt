#include "net/pairing.h"
#include "net/identity.h"
#include "net/membership.h"
#include "net/rendezvous.h"

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace rivt::net {

static std::string hexstr(const uint8_t *d, size_t n) {
    static const char *t = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; i++) { o += t[d[i] >> 4]; o += t[d[i] & 15]; }
    return o;
}

static std::string rand_hex(size_t nbytes) {
    std::string b(nbytes, '\0');
    RAND_bytes((uint8_t *)b.data(), (int)nbytes);
    return hexstr((const uint8_t *)b.data(), nbytes);
}

static std::string hmac_hex(const std::string &key, const std::string &msg) {
    uint8_t out[SHA256_DIGEST_LENGTH];
    unsigned len = 0;
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         (const uint8_t *)msg.data(), msg.size(), out, &len);
    return hexstr(out, len);
}

static std::string sha256_hex(const std::string &d) {
    uint8_t h[SHA256_DIGEST_LENGTH];
    SHA256((const uint8_t *)d.data(), d.size(), h);
    return hexstr(h, sizeof h);
}

// Minimal flat-JSON string field extractor (values are base64/hex/plain,
// no escaping needed for our payloads).
static std::string jget(const std::string &j, const std::string &key) {
    auto k = j.find("\"" + key + "\":\"");
    if (k == std::string::npos) return {};
    k += key.size() + 4;
    auto e = j.find('"', k);
    return e == std::string::npos ? std::string{} : j.substr(k, e - k);
}

static std::string device_name() {
    char host[256] = "rivt";
    gethostname(host, sizeof host - 1);
    std::string n = host;
    auto dot = n.find('.');
    if (dot != std::string::npos) n.resize(dot);
    return n;
}

// code = "rivt1_" + b64( rdv "\n" set_id "\n" invite_hex "\n" secret_hex )
static std::string make_code(const std::string &rdv, const std::string &set,
                             const std::string &invite, const std::string &secret) {
    return "rivt1_" + b64_encode(rdv + "\n" + set + "\n" + invite + "\n" + secret);
}
static bool parse_code(const std::string &code, std::string &rdv, std::string &set,
                       std::string &invite, std::string &secret) {
    if (code.rfind("rivt1_", 0) != 0) return false;
    std::string blob = b64_decode(code.substr(6));
    size_t p0 = blob.find('\n'), p1 = blob.find('\n', p0 + 1), p2 = blob.find('\n', p1 + 1);
    if (p0 == std::string::npos || p1 == std::string::npos || p2 == std::string::npos) return false;
    rdv = blob.substr(0, p0);
    set = blob.substr(p0 + 1, p1 - p0 - 1);
    invite = blob.substr(p1 + 1, p2 - p1 - 1);
    secret = blob.substr(p2 + 1);
    return true;
}

static bool poll_mailbox(const std::string &rdv, const std::string &invite,
                         const std::string &box, std::string &out, int timeout_s) {
    for (int i = 0; i < timeout_s * 2; i++) {
        if (pair_get(rdv, invite, box, out)) return true;
        usleep(500000);
    }
    return false;
}

bool pair_invite(const Identity &self) {
    std::string rdv = rendezvous_url();
    if (rdv.empty()) {
        fprintf(stderr, "rivt: no rendezvous configured "
                        "(~/.config/rivt/rendezvous or $RIVT_RENDEZVOUS)\n");
        return false;
    }
    // Ensure we belong to a set (found a solo one if needed).
    std::string set = sync_membership(self, /*found_if_missing=*/true);
    if (set.empty()) return false;

    std::string invite = rand_hex(16);
    std::string secret = rand_hex(16);
    printf("Pairing code (valid 10 min, single use):\n\n    %s\n\n"
           "Run `rivt join <code>` (or `rivtd join <code>`) on the other device.\n"
           "Waiting for it to connect...\n",
           make_code(rdv, set, invite, secret).c_str());
    fflush(stdout);

    std::string offer_b64;
    if (!poll_mailbox(rdv, invite, "offer", offer_b64, 600)) {
        fprintf(stderr, "rivt: pairing timed out\n");
        return false;
    }
    std::string offer = b64_decode(offer_b64);
    std::string pubkey = b64_decode(jget(offer, "pubkey"));
    std::string cert = b64_decode(jget(offer, "cert"));
    std::string name = jget(offer, "name");
    std::string tag = jget(offer, "hmac");
    if (pubkey.empty() || cert.empty() ||
        hmac_hex(secret, "offer" + pubkey + name + cert) != tag) {
        fprintf(stderr, "rivt: pairing offer failed verification (wrong code?)\n");
        return false;
    }

    printf("\nDevice wants to join:\n    name: %s\n    fingerprint: %s\n\nApprove? [y/N] ",
           name.c_str(), sha256_hex(pubkey).c_str());
    fflush(stdout);
    bool yes = getenv("RIVT_PAIR_YES");
    if (!yes) {
        char line[16] = {0};
        if (fgets(line, sizeof line, stdin)) yes = (line[0] == 'y' || line[0] == 'Y');
    } else {
        printf("y (RIVT_PAIR_YES)\n");
    }
    if (!yes) { fprintf(stderr, "rivt: pairing declined\n"); return false; }

    MembershipLog log;
    if (!log.load_file(MembershipLog::default_path())) return false;
    std::string op = log.add_member(self, pubkey, name, cert);
    if (op.empty()) { fprintf(stderr, "rivt: failed to sign add-op\n"); return false; }
    log.save(MembershipLog::default_path());
    // Push the whole log so the DO (and the joiner) get every op.
    std::vector<std::string> have;
    membership_fetch(rdv, set, have);
    for (size_t i = have.size(); i < log.size(); i++)
        membership_push(rdv, set, (uint32_t)i, log.ops()[i]);
    log.write_bundle(Identity::authorized_bundle_path(), (int64_t)time(nullptr));

    std::string concat;
    std::string ops_json;
    for (size_t i = 0; i < log.size(); i++) {
        concat += log.ops()[i];
        ops_json += (i ? "," : "") + std::string("\"") + b64_encode(log.ops()[i]) + "\"";
    }
    std::string answer = "{\"ops\":[" + ops_json + "],\"hmac\":\"" +
                         hmac_hex(secret, "answer" + set + sha256_hex(concat)) + "\"}";
    if (!pair_put(rdv, invite, "answer", b64_encode(answer))) {
        fprintf(stderr, "rivt: failed to send approval\n");
        return false;
    }
    printf("Approved '%s'. It is now a member of the set.\n", name.c_str());
    return true;
}

bool pair_join(const std::string &code, const Identity &self) {
    std::string rdv, set, invite, secret;
    if (!parse_code(code, rdv, set, invite, secret)) {
        fprintf(stderr, "rivt: malformed pairing code\n");
        return false;
    }
    // The code carries the rendezvous URL, so a joining box needs no
    // prior config — persist it now.
    set_rendezvous_url(rdv);
    std::string name = device_name();
    std::string pubkey = self.spki_der();
    std::string cert = self.cert_pem();
    std::string offer = "{\"pubkey\":\"" + b64_encode(pubkey) + "\",\"cert\":\"" +
                        b64_encode(cert) + "\",\"name\":\"" + name + "\",\"hmac\":\"" +
                        hmac_hex(secret, "offer" + pubkey + name + cert) + "\"}";
    if (!pair_put(rdv, invite, "offer", b64_encode(offer))) {
        fprintf(stderr, "rivt: cannot reach rendezvous\n");
        return false;
    }
    printf("Sent join request as '%s' (fingerprint %s).\n"
           "Waiting for approval on the other device...\n",
           name.c_str(), self.fingerprint().c_str());
    fflush(stdout);

    std::string answer_b64;
    if (!poll_mailbox(rdv, invite, "answer", answer_b64, 600)) {
        fprintf(stderr, "rivt: pairing timed out (not approved)\n");
        return false;
    }
    std::string answer = b64_decode(answer_b64);
    std::string tag = jget(answer, "hmac");

    // Extract the ops array.
    std::vector<std::string> ops;
    auto a = answer.find("\"ops\":[");
    if (a == std::string::npos) return false;
    a += 7;
    size_t p = a;
    while ((p = answer.find('"', p)) != std::string::npos && p < answer.find(']', a)) {
        auto e = answer.find('"', p + 1);
        ops.push_back(b64_decode(answer.substr(p + 1, e - p - 1)));
        p = e + 1;
    }
    std::string concat;
    for (auto &o : ops) concat += o;
    if (hmac_hex(secret, "answer" + set + sha256_hex(concat)) != tag) {
        fprintf(stderr, "rivt: approval failed verification\n");
        return false;
    }

    MembershipLog log;
    if (!log.load(ops) || log.set_id() != set || !log.is_member(pubkey)) {
        fprintf(stderr, "rivt: received an invalid membership log\n");
        return false;
    }
    log.save(MembershipLog::default_path());
    log.write_bundle(Identity::authorized_bundle_path(), (int64_t)time(nullptr));
    printf("Joined set %s (%zu members). This device is now trusted.\n",
           set.c_str(), log.members((int64_t)time(nullptr)).size());
    return true;
}

} // namespace rivt::net
