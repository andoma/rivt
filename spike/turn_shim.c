// QUIC-over-TURN shim pair (spike item 1b). Shares the wire logic of
// turn_testbed.c; kept standalone on purpose (spike code).
//
//   turn_shim client <listen_port>
//     Binds a WAN socket, STUNs, prints "REFLEXIVE ip:port".
//     Reads the relayed address as one "ip:port" line on stdin, then
//     forwards raw datagrams 127.0.0.1:<listen_port> <-> relayed addr.
//
//   turn_shim server <client_refl ip:port> <target ip:port>
//     Allocates on Cloudflare TURN (env TURN_USER/TURN_PASS), creates a
//     permission for the client's reflexive address, prints
//     "RELAYED ip:port", then forwards: Data indications -> target,
//     target replies -> Send indications to the most recent peer
//     address seen (so a client address change is followed, as in the
//     roaming design). Refreshes allocation and permission in-loop.
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x2112A442u
#define A_MAPPED 0x0020
#define A_USER 0x0006
#define A_MI 0x0008
#define A_ERROR 0x0009
#define A_REALM 0x0014
#define A_NONCE 0x0015
#define A_XPEER 0x0012
#define A_DATA 0x0013
#define A_LIFETIME 0x000D
#define A_XRELAY 0x0016
#define A_REQTRANS 0x0019

struct msg { uint8_t buf[2048]; size_t len; };

static void msg_init(struct msg *m, uint16_t type) {
    memset(m->buf, 0, 20);
    m->buf[0] = type >> 8; m->buf[1] = type & 0xff;
    uint32_t magic = htonl(MAGIC);
    memcpy(m->buf + 4, &magic, 4);
    if (getrandom(m->buf + 8, 12, 0) != 12) { perror("getrandom"); exit(1); }
    m->len = 20;
}
static void set_len(struct msg *m, size_t len) {
    m->buf[2] = (len - 20) >> 8; m->buf[3] = (len - 20) & 0xff;
}
static void add_attr(struct msg *m, uint16_t type, const void *val, uint16_t vlen) {
    uint8_t *p = m->buf + m->len;
    p[0] = type >> 8; p[1] = type & 0xff;
    p[2] = vlen >> 8; p[3] = vlen & 0xff;
    memcpy(p + 4, val, vlen);
    size_t pad = (4 - (vlen & 3)) & 3;
    memset(p + 4 + vlen, 0, pad);
    m->len += 4 + vlen + pad;
    set_len(m, m->len);
}
static void add_xor_addr(struct msg *m, uint16_t type, const struct sockaddr_in *sa) {
    uint8_t v[8] = {0, 0x01};
    uint16_t xport = ntohs(sa->sin_port) ^ (uint16_t)(MAGIC >> 16);
    uint32_t xaddr = ntohl(sa->sin_addr.s_addr) ^ MAGIC;
    v[2] = xport >> 8; v[3] = xport & 0xff;
    v[4] = xaddr >> 24; v[5] = xaddr >> 16; v[6] = xaddr >> 8; v[7] = xaddr;
    add_attr(m, type, v, 8);
}
static void add_mi(struct msg *m, const uint8_t key[16]) {
    set_len(m, m->len + 24);
    unsigned int hlen = 20;
    uint8_t hash[20];
    HMAC(EVP_sha1(), key, 16, m->buf, m->len, hash, &hlen);
    add_attr(m, A_MI, hash, 20);
}
static const uint8_t *find_attr(const uint8_t *buf, size_t n, uint16_t want, uint16_t *vlen) {
    size_t mlen = 20 + ((size_t)buf[2] << 8 | buf[3]);
    if (mlen > n) mlen = n;
    size_t off = 20;
    while (off + 4 <= mlen) {
        uint16_t type = (uint16_t)(buf[off] << 8 | buf[off + 1]);
        uint16_t alen = (uint16_t)(buf[off + 2] << 8 | buf[off + 3]);
        if (off + 4 + alen > mlen) break;
        if (type == want) { *vlen = alen; return buf + off + 4; }
        off += 4 + ((alen + 3u) & ~3u);
    }
    return NULL;
}
static int get_xor_addr(const uint8_t *buf, size_t n, uint16_t type, struct sockaddr_in *out) {
    uint16_t vlen;
    const uint8_t *v = find_attr(buf, n, type, &vlen);
    if (!v || vlen < 8 || v[1] != 0x01) return -1;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)((v[2] << 8 | v[3]) ^ (MAGIC >> 16)));
    out->sin_addr.s_addr = htonl(((uint32_t)v[4] << 24 | v[5] << 16 | v[6] << 8 | v[7]) ^ MAGIC);
    return 0;
}
static const char *addr_str(const struct sockaddr_in *sa) {
    static char s[64];
    snprintf(s, sizeof s, "%s:%u", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
    return s;
}
static int resolve(const char *host, const char *port, struct sockaddr_in *out) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res)) return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}
static int parse_addr(const char *s, struct sockaddr_in *out) {
    char host[64];
    const char *colon = strrchr(s, ':');
    if (!colon || (size_t)(colon - s) >= sizeof host) return -1;
    memcpy(host, s, colon - s);
    host[colon - s] = 0;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_port = htons(atoi(colon + 1));
    return inet_pton(AF_INET, host, &out->sin_addr) == 1 ? 0 : -1;
}
static ssize_t request(int fd, struct sockaddr_in *dst, struct msg *req, uint8_t *resp, size_t rlen) {
    for (int attempt = 0; attempt < 3; attempt++) {
        sendto(fd, req->buf, req->len, 0, (struct sockaddr *)dst, sizeof *dst);
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (;;) {
            ssize_t n = recv(fd, resp, rlen, 0);
            if (n < 0) break;
            if (n >= 20 && !memcmp(resp + 8, req->buf + 8, 12)) return n;
        }
    }
    return -1;
}
static void die_error(const char *what, const uint8_t *buf, size_t n) {
    uint16_t vlen;
    const uint8_t *e = find_attr(buf, n, A_ERROR, &vlen);
    if (e && vlen >= 4)
        fprintf(stderr, "%s: error %u%02u %.*s\n", what, e[2] & 7, e[3], vlen - 4, e + 4);
    else
        fprintf(stderr, "%s: unexpected response type %02x%02x\n", what, buf[0], buf[1]);
    exit(1);
}
static int stun_reflexive(int fd, struct sockaddr_in *out) {
    struct sockaddr_in stun;
    if (resolve("stun.cloudflare.com", "3478", &stun)) return -1;
    struct msg m; msg_init(&m, 0x0001);
    uint8_t resp[512];
    ssize_t n = request(fd, &stun, &m, resp, sizeof resp);
    if (n < 0 || resp[0] != 0x01 || resp[1] != 0x01) return -1;
    return get_xor_addr(resp, n, A_MAPPED, out);
}

// ------------------------------ client mode ------------------------------
static int run_client(int listen_port) {
    int W = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in refl;
    if (stun_reflexive(W, &refl)) { fprintf(stderr, "stun failed\n"); return 1; }
    printf("REFLEXIVE %s\n", addr_str(&refl));
    fflush(stdout);

    char line[128];
    if (!fgets(line, sizeof line, stdin)) { fprintf(stderr, "no relay addr on stdin\n"); return 1; }
    line[strcspn(line, "\r\n")] = 0;
    struct sockaddr_in relay;
    if (parse_addr(line, &relay)) { fprintf(stderr, "bad relay addr '%s'\n", line); return 1; }
    fprintf(stderr, "client-shim: forwarding 127.0.0.1:%d <-> %s\n", listen_port, addr_str(&relay));

    int L = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET;
    la.sin_port = htons(listen_port);
    inet_pton(AF_INET, "127.0.0.1", &la.sin_addr);
    if (bind(L, (struct sockaddr *)&la, sizeof la)) { perror("bind"); return 1; }

    struct timeval no = {0, 0};
    setsockopt(W, SOL_SOCKET, SO_RCVTIMEO, &no, sizeof no);
    struct sockaddr_in app = {0};  // local QUIC client, learned on first packet
    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(L, &rf); FD_SET(W, &rf);
        if (select((L > W ? L : W) + 1, &rf, NULL, NULL, NULL) <= 0) continue;
        uint8_t b[2048];
        if (FD_ISSET(L, &rf)) {
            socklen_t al = sizeof app;
            ssize_t n = recvfrom(L, b, sizeof b, 0, (struct sockaddr *)&app, &al);
            if (n > 0) sendto(W, b, n, 0, (struct sockaddr *)&relay, sizeof relay);
        }
        if (FD_ISSET(W, &rf)) {
            ssize_t n = recv(W, b, sizeof b, 0);
            if (n > 0 && app.sin_port)
                sendto(L, b, n, 0, (struct sockaddr *)&app, sizeof app);
        }
    }
}

// ------------------------------ server mode ------------------------------
static const char *g_user;
static char g_realm[128], g_nonce[256];
static uint8_t g_key[16];

static void auth_attrs(struct msg *m) {
    add_attr(m, A_USER, g_user, strlen(g_user));
    add_attr(m, A_REALM, g_realm, strlen(g_realm));
    add_attr(m, A_NONCE, g_nonce, strlen(g_nonce));
    add_mi(m, g_key);
}

// Build a refresh-type request; returns txid via req->buf[8..19].
static void build_refresh(struct msg *m) {
    msg_init(m, 0x0004);
    uint8_t lt[4] = {0, 0, 600 >> 8, 600 & 0xff};
    add_attr(m, A_LIFETIME, lt, 4);
    auth_attrs(m);
}
static void build_permission(struct msg *m, const struct sockaddr_in *peer) {
    msg_init(m, 0x0008);
    add_xor_addr(m, A_XPEER, peer);
    auth_attrs(m);
}

static int run_server(const char *refl_s, const char *target_s) {
    g_user = getenv("TURN_USER");
    const char *pass = getenv("TURN_PASS");
    if (!g_user || !pass) { fprintf(stderr, "TURN_USER/TURN_PASS not set\n"); return 1; }
    struct sockaddr_in client_refl, target, turn_srv;
    if (parse_addr(refl_s, &client_refl) || parse_addr(target_s, &target)) {
        fprintf(stderr, "bad address argument\n"); return 1;
    }
    if (resolve("turn.cloudflare.com", "3478", &turn_srv)) { fprintf(stderr, "resolve failed\n"); return 1; }

    int T = socket(AF_INET, SOCK_DGRAM, 0);
    uint8_t resp[2048];

    // Allocate: unauthenticated probe for realm+nonce, then for real.
    uint8_t reqtrans[4] = {17, 0, 0, 0};
    struct msg m; msg_init(&m, 0x0003);
    add_attr(&m, A_REQTRANS, reqtrans, 4);
    ssize_t n = request(T, &turn_srv, &m, resp, sizeof resp);
    if (n < 0) { fprintf(stderr, "allocate: no response\n"); return 1; }
    uint16_t rl, nl;
    const uint8_t *realm = find_attr(resp, n, A_REALM, &rl);
    const uint8_t *nonce = find_attr(resp, n, A_NONCE, &nl);
    if (!realm || !nonce) die_error("allocate(unauth)", resp, n);
    snprintf(g_realm, sizeof g_realm, "%.*s", rl, realm);
    snprintf(g_nonce, sizeof g_nonce, "%.*s", nl, nonce);
    {
        char cat[512];
        int cl = snprintf(cat, sizeof cat, "%s:%s:%s", g_user, g_realm, pass);
        unsigned int kl = 16;
        EVP_Digest(cat, cl, g_key, &kl, EVP_md5(), NULL);
    }
    msg_init(&m, 0x0003);
    add_attr(&m, A_REQTRANS, reqtrans, 4);
    auth_attrs(&m);
    n = request(T, &turn_srv, &m, resp, sizeof resp);
    if (n < 0 || !(resp[0] == 0x01 && resp[1] == 0x03)) die_error("allocate", resp, n < 0 ? 0 : n);
    struct sockaddr_in relayed;
    if (get_xor_addr(resp, n, A_XRELAY, &relayed)) { fprintf(stderr, "no relayed addr\n"); return 1; }

    build_permission(&m, &client_refl);
    n = request(T, &turn_srv, &m, resp, sizeof resp);
    if (n < 0 || !(resp[0] == 0x01 && resp[1] == 0x08)) die_error("createpermission", resp, n < 0 ? 0 : n);

    printf("RELAYED %s\n", addr_str(&relayed));
    fflush(stdout);
    fprintf(stderr, "server-shim: %s <-> Data/Send <-> %s\n", target_s, addr_str(&relayed));

    int S = socket(AF_INET, SOCK_DGRAM, 0);
    connect(S, (struct sockaddr *)&target, sizeof target);

    struct sockaddr_in cur_peer = client_refl;
    struct timeval no = {0, 0};
    setsockopt(T, SOL_SOCKET, SO_RCVTIMEO, &no, sizeof no);
    time_t next_refresh = time(NULL) + 240;

    for (;;) {
        fd_set rf; FD_ZERO(&rf); FD_SET(T, &rf); FD_SET(S, &rf);
        struct timeval tv = {1, 0};
        int rc = select((T > S ? T : S) + 1, &rf, NULL, NULL, &tv);
        if (rc < 0) { if (errno == EINTR) continue; perror("select"); return 1; }

        if (time(NULL) >= next_refresh) {
            // Blocking refreshes; QUIC retransmission covers the sub-second gap.
            build_refresh(&m);
            n = request(T, &turn_srv, &m, resp, sizeof resp);
            if (n > 0 && resp[0] == 0x01 && resp[1] == 0x14) {  // 438 etc: refresh error
                uint16_t nn;
                const uint8_t *nv = find_attr(resp, n, A_NONCE, &nn);
                if (nv) { snprintf(g_nonce, sizeof g_nonce, "%.*s", nn, nv);
                          build_refresh(&m); request(T, &turn_srv, &m, resp, sizeof resp); }
            }
            build_permission(&m, &cur_peer);
            request(T, &turn_srv, &m, resp, sizeof resp);
            next_refresh = time(NULL) + 240;
            fprintf(stderr, "server-shim: refreshed allocation+permission\n");
        }
        if (rc == 0) continue;

        uint8_t b[2048];
        if (FD_ISSET(T, &rf)) {
            ssize_t r = recv(T, b, sizeof b, 0);
            if (r >= 20 && b[0] == 0x00 && b[1] == 0x17) {  // Data indication
                struct sockaddr_in peer;
                uint16_t dl;
                const uint8_t *d = find_attr(b, r, A_DATA, &dl);
                if (d && !get_xor_addr(b, r, A_XPEER, &peer)) {
                    if (memcmp(&peer, &cur_peer, sizeof peer)) {
                        fprintf(stderr, "server-shim: peer moved %s", addr_str(&cur_peer));
                        fprintf(stderr, " -> %s\n", addr_str(&peer));
                        cur_peer = peer;
                        build_permission(&m, &cur_peer);
                        request(T, &turn_srv, &m, resp, sizeof resp);
                    }
                    send(S, d, dl, 0);
                }
            }
        }
        if (FD_ISSET(S, &rf)) {
            ssize_t r = recv(S, b, sizeof b, 0);
            if (r > 0) {
                struct msg si;
                msg_init(&si, 0x0016);
                add_xor_addr(&si, A_XPEER, &cur_peer);
                add_attr(&si, A_DATA, b, r);
                sendto(T, si.buf, si.len, 0, (struct sockaddr *)&turn_srv, sizeof turn_srv);
            }
        }
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "client")) return run_client(atoi(argv[2]));
    if (argc >= 4 && !strcmp(argv[1], "server")) return run_server(argv[2], argv[3]);
    fprintf(stderr, "usage: turn_shim client <listen_port>\n"
                    "       turn_shim server <client_refl ip:port> <target ip:port>\n");
    return 1;
}
