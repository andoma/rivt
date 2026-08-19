// TURN-over-UDP testbed against Cloudflare (RFC 8656 subset, IPv4).
//
//   turn_testbed selftest   — one process, two sockets:
//     T = TURN client: Allocate + CreatePermission(E's reflexive addr)
//     E = plain UDP peer: STUN for reflexive, punch toward relayed addr, echo
//     Then probe increasing datagram sizes T -> relay -> E -> relay -> T.
//
// Env: TURN_USER, TURN_PASS (from mint-creds.sh).
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
#include <sys/time.h>
#include <unistd.h>

#define MAGIC 0x2112A442u
#define A_MAPPED  0x0020
#define A_USER    0x0006
#define A_MI      0x0008
#define A_ERROR   0x0009
#define A_REALM   0x0014
#define A_NONCE   0x0015
#define A_XPEER   0x0012
#define A_DATA    0x0013
#define A_LIFETIME 0x000D
#define A_XRELAY  0x0016
#define A_REQTRANS 0x0019

static double now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1e3 + tv.tv_usec / 1e3;
}

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

static void add_xor_addr(struct msg *m, uint16_t type, struct sockaddr_in *sa) {
    uint8_t v[8] = {0, 0x01};
    uint16_t xport = ntohs(sa->sin_port) ^ (uint16_t)(MAGIC >> 16);
    uint32_t xaddr = ntohl(sa->sin_addr.s_addr) ^ MAGIC;
    v[2] = xport >> 8; v[3] = xport & 0xff;
    v[4] = xaddr >> 24; v[5] = xaddr >> 16; v[6] = xaddr >> 8; v[7] = xaddr;
    add_attr(m, type, v, 8);
}

// key = MD5(user:realm:pass); MI = HMAC-SHA1 over message with length
// pre-adjusted to include the MI attribute.
static void add_mi(struct msg *m, const uint8_t key[16]) {
    set_len(m, m->len + 24);
    unsigned int hlen = 20;
    uint8_t hash[20];
    HMAC(EVP_sha1(), key, 16, m->buf, m->len, hash, &hlen);
    add_attr(m, A_MI, hash, 20);
}

// Iterate attributes; returns value ptr or NULL.
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

static const char *addr_str(struct sockaddr_in *sa) {
    static char s[64];
    snprintf(s, sizeof s, "%s:%u", inet_ntoa(sa->sin_addr), ntohs(sa->sin_port));
    return s;
}

// Send request, await response with matching txid. 3 attempts, 2s each.
static ssize_t request(int fd, struct sockaddr_in *dst, struct msg *req, uint8_t *resp, size_t rlen) {
    for (int attempt = 0; attempt < 3; attempt++) {
        sendto(fd, req->buf, req->len, 0, (struct sockaddr *)dst, sizeof *dst);
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (;;) {
            ssize_t n = recv(fd, resp, rlen, 0);
            if (n < 0) break;  // timeout -> retransmit
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

static int resolve(const char *host, const char *port, struct sockaddr_in *out) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res)) return -1;
    memcpy(out, res->ai_addr, sizeof *out);
    freeaddrinfo(res);
    return 0;
}

// STUN Binding on an existing socket; fills reflexive addr.
static int stun_bind(int fd, struct sockaddr_in *out) {
    struct sockaddr_in stun;
    if (resolve("stun.cloudflare.com", "3478", &stun)) return -1;
    struct msg m; msg_init(&m, 0x0001);
    uint8_t resp[512];
    ssize_t n = request(fd, &stun, &m, resp, sizeof resp);
    if (n < 0 || resp[0] != 0x01 || resp[1] != 0x01) return -1;
    return get_xor_addr(resp, n, A_MAPPED, out);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *user = getenv("TURN_USER"), *pass = getenv("TURN_PASS");
    if (!user || !pass) { fprintf(stderr, "TURN_USER/TURN_PASS not set\n"); return 1; }

    struct sockaddr_in turn_srv;
    if (resolve("turn.cloudflare.com", "3478", &turn_srv)) { fprintf(stderr, "resolve failed\n"); return 1; }
    printf("turn server: %s\n", addr_str(&turn_srv));

    int T = socket(AF_INET, SOCK_DGRAM, 0);
    int E = socket(AF_INET, SOCK_DGRAM, 0);

    // --- E: discover reflexive address ---
    struct sockaddr_in e_refl;
    if (stun_bind(E, &e_refl)) { fprintf(stderr, "E stun failed\n"); return 1; }
    printf("E reflexive: %s\n", addr_str(&e_refl));

    // --- T: Allocate (expect 401, then authenticate) ---
    uint8_t reqtrans[4] = {17, 0, 0, 0};
    struct msg m; msg_init(&m, 0x0003);
    add_attr(&m, A_REQTRANS, reqtrans, 4);
    uint8_t resp[2048];
    ssize_t n = request(T, &turn_srv, &m, resp, sizeof resp);
    if (n < 0) { fprintf(stderr, "allocate: no response\n"); return 1; }
    uint16_t realm_len, nonce_len;
    const uint8_t *realm = find_attr(resp, n, A_REALM, &realm_len);
    const uint8_t *nonce = find_attr(resp, n, A_NONCE, &nonce_len);
    if (!(resp[0] == 0x01 && resp[1] == 0x13) || !realm || !nonce)
        die_error("allocate(unauth)", resp, n);
    char realm_s[128], nonce_s[256];
    snprintf(realm_s, sizeof realm_s, "%.*s", realm_len, realm);
    snprintf(nonce_s, sizeof nonce_s, "%.*s", nonce_len, nonce);
    printf("realm: %s\n", realm_s);

    uint8_t key[16];
    {
        char cat[512];
        int cl = snprintf(cat, sizeof cat, "%s:%s:%s", user, realm_s, pass);
        unsigned int kl = 16;
        EVP_Digest(cat, cl, key, &kl, EVP_md5(), NULL);
    }

    msg_init(&m, 0x0003);
    add_attr(&m, A_REQTRANS, reqtrans, 4);
    add_attr(&m, A_USER, user, strlen(user));
    add_attr(&m, A_REALM, realm_s, strlen(realm_s));
    add_attr(&m, A_NONCE, nonce_s, strlen(nonce_s));
    add_mi(&m, key);
    n = request(T, &turn_srv, &m, resp, sizeof resp);
    if (n < 0 || !(resp[0] == 0x01 && resp[1] == 0x03)) die_error("allocate", resp, n < 0 ? 0 : n);

    struct sockaddr_in relayed, t_refl;
    if (get_xor_addr(resp, n, A_XRELAY, &relayed)) { fprintf(stderr, "no relayed addr\n"); return 1; }
    get_xor_addr(resp, n, A_MAPPED, &t_refl);
    uint16_t ll; const uint8_t *lt = find_attr(resp, n, A_LIFETIME, &ll);
    printf("T reflexive: %s\n", addr_str(&t_refl));
    printf("relayed:     %s\n", addr_str(&relayed));
    if (lt && ll == 4)
        printf("lifetime:    %us\n", (unsigned)(lt[0] << 24 | lt[1] << 16 | lt[2] << 8 | lt[3]));

    // --- T: CreatePermission for E's reflexive address ---
    msg_init(&m, 0x0008);
    add_xor_addr(&m, A_XPEER, &e_refl);
    add_attr(&m, A_USER, user, strlen(user));
    add_attr(&m, A_REALM, realm_s, strlen(realm_s));
    add_attr(&m, A_NONCE, nonce_s, strlen(nonce_s));
    add_mi(&m, key);
    n = request(T, &turn_srv, &m, resp, sizeof resp);
    if (n < 0 || !(resp[0] == 0x01 && resp[1] == 0x08)) die_error("createpermission", resp, n < 0 ? 0 : n);
    printf("permission:  ok (for %s)\n", addr_str(&e_refl));

    // --- E: punch toward relayed address until T sees the hello ---
    int punched = 0;
    for (int i = 0; i < 10 && !punched; i++) {
        sendto(E, "hello", 5, 0, (struct sockaddr *)&relayed, sizeof relayed);
        struct timeval tv = {1, 0};
        setsockopt(T, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t r = recv(T, resp, sizeof resp, 0);
        if (r >= 20 && resp[0] == 0x00 && resp[1] == 0x17) punched = 1;  // Data indication
    }
    if (!punched) { fprintf(stderr, "punch failed: no Data indication\n"); return 1; }
    printf("punch:       ok (E -> relay -> T data path up)\n\n");

    // --- MTU / size probes: T --Send--> relay --raw--> E --raw echo--> relay --Data--> T ---
    // Send indication overhead: 20 hdr + 12 XOR-PEER-ADDRESS + 4 DATA hdr = 36 bytes.
    static const int sizes[] = {256, 1000, 1200, 1232, 1280, 1350, 1400, 1436, 1444, 1452, 1472};
    printf("%-6s %-4s %s\n", "size", "ok", "rtt");
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int sz = sizes[i], got = 0;
        double rtt = 0;
        for (int attempt = 0; attempt < 3 && !got; attempt++) {
            uint8_t payload[2048];
            memset(payload, 0x42, sz);
            payload[0] = i;  // tag
            msg_init(&m, 0x0016);  // Send indication
            add_xor_addr(&m, A_XPEER, &e_refl);
            add_attr(&m, A_DATA, payload, sz);
            double t0 = now_ms();
            sendto(T, m.buf, m.len, 0, (struct sockaddr *)&turn_srv, sizeof turn_srv);

            // Pump both sockets up to 2s: E echoes raw, T awaits Data indication.
            double deadline = t0 + 2000;
            while (now_ms() < deadline && !got) {
                fd_set rf; FD_ZERO(&rf); FD_SET(T, &rf); FD_SET(E, &rf);
                struct timeval tv = {0, 100000};
                if (select((T > E ? T : E) + 1, &rf, NULL, NULL, &tv) <= 0) continue;
                if (FD_ISSET(E, &rf)) {
                    struct sockaddr_in from; socklen_t fl = sizeof from;
                    uint8_t b[2048];
                    ssize_t r = recvfrom(E, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
                    if (r > 0) sendto(E, b, r, 0, (struct sockaddr *)&from, fl);  // echo
                }
                if (FD_ISSET(T, &rf)) {
                    uint8_t b[2048];
                    ssize_t r = recv(T, b, sizeof b, 0);
                    if (r >= 20 && b[0] == 0x00 && b[1] == 0x17) {
                        uint16_t dl; const uint8_t *d = find_attr(b, r, A_DATA, &dl);
                        if (d && dl == sz && d[0] == (uint8_t)i) { got = 1; rtt = now_ms() - t0; }
                    }
                }
            }
        }
        if (got) printf("%-6d %-4s %.1fms\n", sz, "yes", rtt);
        else     printf("%-6d %-4s -\n", sz, "NO");
    }
    return 0;
}
