#include "net/turn.h"
#include "net/sock.h"
#include "net/stun.h"
#include "core/debug.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>

namespace rivt::net {

static constexpr uint32_t MAGIC = 0x2112A442u;
enum { A_MAPPED = 0x0020, A_USER = 0x0006, A_MI = 0x0008, A_REALM = 0x0014,
       A_NONCE = 0x0015, A_XPEER = 0x0012, A_DATA = 0x0013, A_LIFETIME = 0x000D,
       A_XRELAY = 0x0016, A_REQTRANS = 0x0019 };

namespace {
struct Msg {
    uint8_t buf[2048];
    size_t len = 0;
    void init(uint16_t type) {
        memset(buf, 0, 20);
        buf[0] = type >> 8; buf[1] = type & 0xff;
        uint32_t m = htonl(MAGIC);
        memcpy(buf + 4, &m, 4);
        if (RAND_bytes(buf + 8, 12) != 1)
            for (int i = 0; i < 12; i++) buf[8 + i] = (uint8_t)(i * 7 + 3);
        len = 20;
    }
    void set_body_len(size_t l) { buf[2] = (l - 20) >> 8; buf[3] = (l - 20) & 0xff; }
    void attr(uint16_t type, const void *val, uint16_t vlen) {
        uint8_t *p = buf + len;
        p[0] = type >> 8; p[1] = type & 0xff; p[2] = vlen >> 8; p[3] = vlen & 0xff;
        memcpy(p + 4, val, vlen);
        size_t pad = (4 - (vlen & 3)) & 3;
        memset(p + 4 + vlen, 0, pad);
        len += 4 + vlen + pad;
        set_body_len(len);
    }
    void xor_addr(uint16_t type, const struct sockaddr_in *sa) {
        uint8_t v[8] = {0, 0x01};
        uint16_t xp = ntohs(sa->sin_port) ^ (uint16_t)(MAGIC >> 16);
        uint32_t xa = ntohl(sa->sin_addr.s_addr) ^ MAGIC;
        v[2] = xp >> 8; v[3] = xp & 0xff;
        v[4] = xa >> 24; v[5] = xa >> 16; v[6] = xa >> 8; v[7] = xa;
        attr(type, v, 8);
    }
    void integrity(const uint8_t key[16]) {
        set_body_len(len + 24);
        unsigned hl = 20;
        uint8_t hash[20];
        HMAC(EVP_sha1(), key, 16, buf, len, hash, &hl);
        attr(A_MI, hash, 20);
    }
};

const uint8_t *find_attr(const uint8_t *buf, size_t n, uint16_t want, uint16_t *vlen) {
    size_t mlen = 20 + ((size_t)buf[2] << 8 | buf[3]);
    if (mlen > n) mlen = n;
    size_t off = 20;
    while (off + 4 <= mlen) {
        uint16_t t = (uint16_t)(buf[off] << 8 | buf[off + 1]);
        uint16_t al = (uint16_t)(buf[off + 2] << 8 | buf[off + 3]);
        if (off + 4 + al > mlen) break;
        if (t == want) { *vlen = al; return buf + off + 4; }
        off += 4 + ((al + 3u) & ~3u);
    }
    return nullptr;
}

const char *stun_error_name(int code) {
    switch (code) {
        case 401: return "unauthorized";
        case 403: return "forbidden";
        case 437: return "allocation mismatch";
        case 438: return "stale nonce";
        case 441: return "wrong credentials";
        case 486: return "allocation quota reached";
        case 508: return "insufficient capacity";
        default:  return "?";
    }
}

// STUN ERROR-CODE attribute -> numeric code (e.g. 438), 0 if absent.
int stun_error_code(const uint8_t *buf, size_t n) {
    uint16_t vl;
    const uint8_t *v = find_attr(buf, n, 0x0009, &vl);
    if (!v || vl < 4) return 0;
    return (v[2] & 0x07) * 100 + v[3];
}

bool get_xor_addr(const uint8_t *buf, size_t n, uint16_t type, struct sockaddr_in *out) {
    uint16_t vl;
    const uint8_t *v = find_attr(buf, n, type, &vl);
    if (!v || vl < 8 || v[1] != 0x01) return false;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)((v[2] << 8 | v[3]) ^ (MAGIC >> 16)));
    uint32_t xa = (uint32_t)v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7];
    out->sin_addr.s_addr = htonl(xa ^ MAGIC);
    return true;
}
}  // namespace

TurnRelay::~TurnRelay() {
    if (m_refresh_timer >= 0) m_loop.remove_timer(m_refresh_timer);
    if (m_keepalive_timer >= 0) m_loop.remove_timer(m_keepalive_timer);
    if (m_fd >= 0) { m_loop.remove_fd(m_fd); ::close(m_fd); }
}

// Blocking send + await matching response (3 tries, 2s each). The
// socket is always O_NONBLOCK; poll() does the waiting. (SO_RCVTIMEO
// was used before, which is a no-op on a non-blocking socket — after
// allocate() flipped the socket non-blocking, every recv failed
// instantly with EAGAIN and all refresh/permit responses were lost.)
// Relayed traffic shares this socket: non-matching Data indications are
// dispatched, not eaten.
bool TurnRelay::request(uint16_t type, bool with_auth, const struct sockaddr_in *peer,
                        uint8_t *resp, size_t *resp_len, int attempts) {
    Msg m;
    m.init(type);
    if (type == 0x0003) { uint8_t rt[4] = {17, 0, 0, 0}; m.attr(A_REQTRANS, rt, 4); }
    if (type == 0x0004) { uint8_t lt[4] = {0, 0, 600 >> 8, 600 & 0xff}; m.attr(A_LIFETIME, lt, 4); }
    if (peer) m.xor_addr(A_XPEER, peer);
    if (with_auth) {
        m.attr(A_USER, m_user.data(), m_user.size());
        m.attr(A_REALM, m_realm.data(), m_realm.size());
        m.attr(A_NONCE, m_nonce.data(), m_nonce.size());
        m.integrity(m_key);
    }
    for (int attempt = 0; attempt < attempts; attempt++) {
        sendto(m_fd, m.buf, m.len, 0, (struct sockaddr *)&m_server, sizeof m_server);
        for (int waited_ms = 0; waited_ms < 2000;) {
            struct pollfd pf = {m_fd, POLLIN, 0};
            int pr = poll(&pf, 1, 100);
            waited_ms += 100;
            if (pr < 0) break;
            if (pr == 0) continue;
            for (;;) {
                ssize_t n = recv(m_fd, resp, 2048, 0);
                if (n < 0) break;
                if (n >= 20 && !memcmp(resp + 8, m.buf + 8, 12)) { *resp_len = n; return true; }
                dispatch_data_indication(resp, (size_t)n);
            }
        }
    }
    return false;
}

// Inbound Data indication -> on_data. Shared by the async read path and
// request()'s wait loop (relayed QUIC arrives on the same socket).
double TurnRelay::seconds_since_data() const {
    if (m_last_data == std::chrono::steady_clock::time_point{}) return 0.0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_last_data).count();
}

void TurnRelay::dispatch_data_indication(const uint8_t *buf, size_t n) {
    if (n < 20 || buf[0] != 0x00 || buf[1] != 0x17) return;
    m_last_data = std::chrono::steady_clock::now();
    struct sockaddr_in peer {};
    uint16_t dl;
    const uint8_t *d = find_attr(buf, n, A_DATA, &dl);
    if (d && get_xor_addr(buf, n, A_XPEER, &peer) && on_data)
        on_data(peer, d, dl);
}

bool TurnRelay::allocate(const std::string &turn_host, uint16_t turn_port,
                         const std::string &user, const std::string &pass) {
    m_user = user;
    m_pass = pass;
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(turn_host.c_str(), std::to_string(turn_port).c_str(), &hints, &res) != 0 || !res)
        return false;

    // Non-blocking from the start: request() waits via poll(), and the
    // steady-state Data-indication reads must never block the loop.
    m_fd = socket_cloexec(AF_INET, SOCK_DGRAM, 0, /*nonblock=*/true);
    if (m_fd < 0) { freeaddrinfo(res); return false; }

    uint8_t resp[2048];
    size_t rn = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto ms = [t0]() {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    size_t naddr = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
        if (ai->ai_family == AF_INET) naddr++;
    // Unauthenticated Allocate -> 401 with realm+nonce. The host has
    // several A records and one being unreachable is a real occurrence:
    // try each resolved address (2 attempts x 2s apiece) instead of
    // camping on the first — same lesson as the v6-first STUN bug.
    bool got_401 = false;
    size_t tried = 0;
    char ip[INET_ADDRSTRLEN] = {0};
    for (struct addrinfo *ai = res; ai && !got_401; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) continue;
        memcpy(&m_server, ai->ai_addr, sizeof m_server);
        inet_ntop(AF_INET, &m_server.sin_addr, ip, sizeof ip);
        tried++;
        auto ta = std::chrono::steady_clock::now();
        if (request(0x0003, false, nullptr, resp, &rn, 2)) {
            got_401 = true;
            break;
        }
        double waited = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - ta).count();
        rivt::logmsg("turn: allocate: %s (address %zu/%zu of %s) sent 2 requests, "
                     "0 responses in %.1fs — trying next address\n",
                     ip, tried, naddr, turn_host.c_str(), waited);
    }
    freeaddrinfo(res);
    if (!got_401) {
        rivt::logmsg("turn: allocate FAILED: all %zu address(es) of %s silent "
                     "(%d ms total) — network drops UDP/3478, or resolver gave "
                     "dead addresses\n", naddr, turn_host.c_str(), ms());
        return false;
    }
    uint16_t rl, nl;
    const uint8_t *realm = find_attr(resp, rn, A_REALM, &rl);
    const uint8_t *nonce = find_attr(resp, rn, A_NONCE, &nl);
    if (!realm || !nonce) return false;
    m_realm.assign((const char *)realm, rl);
    m_nonce.assign((const char *)nonce, nl);
    std::string cat = m_user + ":" + m_realm + ":" + m_pass;
    unsigned kl = 16;
    EVP_Digest(cat.data(), cat.size(), m_key, &kl, EVP_md5(), nullptr);

    // Authenticated Allocate.
    if (!request(0x0003, true, nullptr, resp, &rn)) {
        rivt::logmsg("turn: allocate FAILED: %s answered the 401 challenge but went "
                     "silent on the authenticated Allocate (%d ms in)\n", ip, ms());
        return false;
    }
    if (!(resp[0] == 0x01 && resp[1] == 0x03)) {  // Allocate Success
        int err = stun_error_code(resp, rn);
        rivt::logmsg("turn: allocate FAILED: %s rejected credentials/allocation "
                     "(error %d: %s)\n", ip, err, stun_error_name(err));
        return false;
    }
    struct sockaddr_in relayed {};
    if (!get_xor_addr(resp, rn, A_XRELAY, &relayed)) return false;
    char rip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &relayed.sin_addr, rip, sizeof rip);
    m_relayed_host = rip;
    m_relayed_port = ntohs(relayed.sin_port);

    m_last_data = std::chrono::steady_clock::now();
    // Steady state: async Data indications; periodic refresh.
    m_loop.add_fd(m_fd, [this](uint32_t ev) { if (ev & EV_READ) on_socket(); });
    m_refresh_timer = m_loop.add_timer(240000, [this]() { refresh(); }, true);
    // NAT keepalive: the 240s refresh cadence is far above typical NAT
    // UDP timeouts (30-180s), so on an idle relay our NAT mapping toward
    // the TURN server expires between refreshes. Control still works
    // (each refresh re-opens the mapping) but inbound Data indications
    // arriving in the dead window are dropped — a zombie relay that
    // looks healthy. A cheap unauthenticated Binding request every 20s
    // keeps the mapping open in both directions (fire-and-forget; the
    // response is ignored by the demux). RIVT_TURN_NO_KEEPALIVE=1
    // disables it for A/B testing.
    if (!getenv("RIVT_TURN_NO_KEEPALIVE")) {
        m_keepalive_timer = m_loop.add_timer(20000, [this]() {
            uint8_t req[20];
            uint8_t txid[STUN_TXID_LEN];
            stun_build_request(req, txid);
            sendto(m_fd, req, sizeof req, 0, (struct sockaddr *)&m_server,
                   sizeof m_server);
        }, true);
    }
    rivt::logmsg("turn: relay allocated %s:%u via %s (%d ms, lifetime 600s, "
                 "refresh 240s)\n", m_relayed_host.c_str(), m_relayed_port, ip, ms());
    return true;
}

void TurnRelay::refresh() {
    uint8_t resp[2048];
    size_t rn = 0;
    // Refresh the allocation; on an error response (438 nonce rotation
    // being the expected one) re-auth once with the fresh nonce.
    bool got = request(0x0004, true, nullptr, resp, &rn);
    bool ok = got;
    if (got && resp[0] == 0x01 && resp[1] == 0x14) {  // Refresh error response
        int err = stun_error_code(resp, rn);
        uint16_t nl;
        const uint8_t *nonce = find_attr(resp, rn, A_NONCE, &nl);
        rivt::logmsg("turn: refresh rejected (error %d)%s\n", err,
                     nonce ? ", re-authenticating with fresh nonce" : "");
        ok = false;
        if (nonce) {
            m_nonce.assign((const char *)nonce, nl);
            got = request(0x0004, true, nullptr, resp, &rn);
            ok = got;
        }
    }
    if (ok) ok = resp[0] == 0x01 && resp[1] == 0x04;  // Refresh Success
    if (!ok) {
        char sip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &m_server.sin_addr, sip, sizeof sip);
        if (!got)
            rivt::logmsg("turn: refresh: %s silent for 3x2s requests\n", sip);
        else if (!(resp[0] == 0x01 && resp[1] == 0x04)) {
            int err = stun_error_code(resp, rn);
            rivt::logmsg("turn: refresh rejected by %s (error %d: %s)\n",
                         sip, err, stun_error_name(err));
        }
        // The allocation is gone (or the server is unreachable): the
        // relayed address is dead and must never be advertised again.
        // A silent failure here once left a daemon advertising a dead
        // relay for hours — and blocking its event loop for minutes per
        // refresh cycle timing out permission requests against it.
        rivt::logmsg("turn: allocation refresh failed — relay %s:%u is dead\n",
                     m_relayed_host.c_str(), m_relayed_port);
        m_alive = false;
        if (m_refresh_timer >= 0) { m_loop.remove_timer(m_refresh_timer); m_refresh_timer = -1; }
        return;
    }
    // Permissions expire at 300s independently of the allocation, so
    // re-issue CreatePermission for every peer or the relay goes silent
    // at ~5 min even though the allocation is still alive.
    size_t renewed = 0;
    for (const auto &p : m_peers)
        if (request(0x0008, true, &p, resp, &rn) && resp[0] == 0x01 && resp[1] == 0x08)
            renewed++;
    rivt::logmsg("turn: allocation refreshed, %zu/%zu permission(s) renewed\n",
                 renewed, m_peers.size());
    // Control-plane success proves nothing about the data path: the
    // owner self-probes through the relayed address every minute, so
    // prolonged Data-indication silence means the relay stopped
    // forwarding — declare it dead so a fresh one gets allocated.
    if (m_expect_probes && seconds_since_data() > 150.0) {
        rivt::logmsg("turn: data path silent %.0fs despite probes — relay %s:%u is dead\n",
                     seconds_since_data(), m_relayed_host.c_str(), m_relayed_port);
        m_alive = false;
        if (m_refresh_timer >= 0) { m_loop.remove_timer(m_refresh_timer); m_refresh_timer = -1; }
        if (m_keepalive_timer >= 0) { m_loop.remove_timer(m_keepalive_timer); m_keepalive_timer = -1; }
    }
}

void TurnRelay::permit(const struct sockaddr_in &peer) {
    if (!m_alive) return;
    // TURN permissions are per IP, port-agnostic (RFC 5766 §9): dedup by
    // address only, or a reconnecting client (two fresh ports per punch)
    // grows this list without bound and refresh() re-issues hundreds of
    // blocking CreatePermissions.
    bool known = false;
    for (const auto &p : m_peers)
        if (p.sin_addr.s_addr == peer.sin_addr.s_addr) { known = true; break; }
    if (!known) {
        uint8_t resp[2048];
        size_t rn = 0;
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        bool got = request(0x0008, true, &peer, resp, &rn);  // CreatePermission
        bool ok = got && resp[0] == 0x01 && resp[1] == 0x08;
        rivt::logmsg("turn: permission for %s: %s\n", ip,
                     ok ? "granted"
                        : got ? "rejected" : "no response");
        // Remember it so refresh() keeps the permission alive past 300s.
        m_peers.push_back(peer);
    }
}

void TurnRelay::send_to(const struct sockaddr_in &peer, const uint8_t *data, size_t len) {
    Msg m;
    m.init(0x0016);  // Send indication (no auth)
    m.xor_addr(A_XPEER, &peer);
    m.attr(A_DATA, data, (uint16_t)len);
    sendto(m_fd, m.buf, m.len, 0, (struct sockaddr *)&m_server, sizeof m_server);
}

void TurnRelay::on_socket() {
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recv(m_fd, buf, sizeof buf, 0);
        if (n < 0) break;
        dispatch_data_indication(buf, (size_t)n);
    }
}

} // namespace rivt::net
