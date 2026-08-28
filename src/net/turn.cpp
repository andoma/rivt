#include "net/turn.h"
#include "net/sock.h"
#include "core/debug.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
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
    if (m_fd >= 0) { m_loop.remove_fd(m_fd); ::close(m_fd); }
}

// Blocking send + await matching response (3 tries, 2s each).
bool TurnRelay::request(uint16_t type, bool with_auth, const struct sockaddr_in *peer,
                        uint8_t *resp, size_t *resp_len) {
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
    for (int attempt = 0; attempt < 3; attempt++) {
        sendto(m_fd, m.buf, m.len, 0, (struct sockaddr *)&m_server, sizeof m_server);
        struct timeval tv = {2, 0};
        setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (;;) {
            ssize_t n = recv(m_fd, resp, 2048, 0);
            if (n < 0) break;
            if (n >= 20 && !memcmp(resp + 8, m.buf + 8, 12)) { *resp_len = n; return true; }
        }
    }
    return false;
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
    memcpy(&m_server, res->ai_addr, sizeof m_server);
    freeaddrinfo(res);

    m_fd = socket_cloexec(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) return false;

    uint8_t resp[2048];
    size_t rn = 0;
    // Unauthenticated Allocate -> 401 with realm+nonce.
    if (!request(0x0003, false, nullptr, resp, &rn)) return false;
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
    if (!request(0x0003, true, nullptr, resp, &rn)) return false;
    if (!(resp[0] == 0x01 && resp[1] == 0x03)) return false;  // Allocate Success
    struct sockaddr_in relayed {};
    if (!get_xor_addr(resp, rn, A_XRELAY, &relayed)) return false;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &relayed.sin_addr, ip, sizeof ip);
    m_relayed_host = ip;
    m_relayed_port = ntohs(relayed.sin_port);

    // Steady state: async Data indications; periodic refresh.
    int fl = fcntl(m_fd, F_GETFL);
    fcntl(m_fd, F_SETFL, fl | O_NONBLOCK);
    struct timeval z = {0, 0};
    setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z);
    m_loop.add_fd(m_fd, [this](uint32_t ev) { if (ev & EV_READ) on_socket(); });
    m_refresh_timer = m_loop.add_timer(240000, [this]() { refresh(); }, true);
    dbg("turn: relay allocated %s:%u (lifetime 600s, refresh 240s)",
        m_relayed_host.c_str(), m_relayed_port);
    return true;
}

void TurnRelay::refresh() {
    uint8_t resp[2048];
    size_t rn = 0;
    // Refresh allocation and (implicitly) re-auth if nonce rotated.
    if (request(0x0004, true, nullptr, resp, &rn) &&
        resp[0] == 0x01 && resp[1] == 0x14) {  // 438 stale nonce style error
        uint16_t nl;
        const uint8_t *nonce = find_attr(resp, rn, A_NONCE, &nl);
        if (nonce) { m_nonce.assign((const char *)nonce, nl); request(0x0004, true, nullptr, resp, &rn); }
    }
    // Permissions expire at 300s independently of the allocation, so
    // re-issue CreatePermission for every peer or the relay goes silent
    // at ~5 min even though the allocation is still alive.
    for (const auto &p : m_peers) request(0x0008, true, &p, resp, &rn);
    dbg("turn: refreshed allocation + %zu permission(s)", m_peers.size());
    // Re-arm non-blocking after the blocking refresh.
    struct timeval z = {0, 0};
    setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z);
}

void TurnRelay::permit(const struct sockaddr_in &peer) {
    uint8_t resp[2048];
    size_t rn = 0;
    request(0x0008, true, &peer, resp, &rn);  // CreatePermission
    // Remember it so refresh() keeps the permission alive past 300s.
    bool known = false;
    for (const auto &p : m_peers)
        if (p.sin_addr.s_addr == peer.sin_addr.s_addr && p.sin_port == peer.sin_port) { known = true; break; }
    if (!known) m_peers.push_back(peer);
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    dbg("turn: permitted peer %s:%u", ip, ntohs(peer.sin_port));
    struct timeval z = {0, 0};
    setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &z, sizeof z);
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
        if (n < 20 || buf[0] != 0x00 || buf[1] != 0x17) continue;  // Data indication only
        struct sockaddr_in peer {};
        uint16_t dl;
        const uint8_t *d = find_attr(buf, n, A_DATA, &dl);
        if (d && get_xor_addr(buf, n, A_XPEER, &peer) && on_data)
            on_data(peer, d, dl);
    }
}

} // namespace rivt::net
