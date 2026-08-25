#include "net/stun.h"

#include <cstring>
#include <netinet/in.h>
#include <sys/random.h>

namespace rivt::net {

static constexpr uint32_t MAGIC = 0x2112A442u;

bool is_stun(const uint8_t *data, size_t len) {
    if (len < 20) return false;
    if (data[0] & 0xc0) return false;  // QUIC fixed bit / long-header bit
    uint32_t cookie;
    memcpy(&cookie, data + 4, 4);
    return ntohl(cookie) == MAGIC;
}

size_t stun_build_request(uint8_t *out, uint8_t txid[STUN_TXID_LEN]) {
    memset(out, 0, 20);
    out[0] = 0x00; out[1] = 0x01;  // Binding Request
    out[2] = 0x00; out[3] = 0x00;  // length 0
    uint32_t magic = htonl(MAGIC);
    memcpy(out + 4, &magic, 4);
    if (getrandom(txid, STUN_TXID_LEN, 0) != (ssize_t)STUN_TXID_LEN) {
        for (size_t i = 0; i < STUN_TXID_LEN; i++) txid[i] = (uint8_t)(i * 7 + 1);
    }
    memcpy(out + 8, txid, STUN_TXID_LEN);
    return 20;
}

bool stun_parse_response(const uint8_t *data, size_t len,
                         const uint8_t txid[STUN_TXID_LEN],
                         struct sockaddr_storage *mapped) {
    if (len < 20 || data[0] != 0x01 || data[1] != 0x01) return false;  // Binding Success
    if (memcmp(data + 8, txid, STUN_TXID_LEN) != 0) return false;

    size_t mlen = 20 + ((size_t)data[2] << 8 | data[3]);
    if (mlen > len) mlen = len;
    size_t off = 20;
    while (off + 4 <= mlen) {
        uint16_t type = (uint16_t)(data[off] << 8 | data[off + 1]);
        uint16_t alen = (uint16_t)(data[off + 2] << 8 | data[off + 3]);
        const uint8_t *v = data + off + 4;
        if (off + 4 + alen > mlen) break;
        if (type == 0x0020 && alen >= 8) {  // XOR-MAPPED-ADDRESS
            uint16_t xport = (uint16_t)(v[2] << 8 | v[3]);
            uint16_t port = xport ^ (uint16_t)(MAGIC >> 16);
            if (v[1] == 0x01) {  // IPv4
                auto *s = (struct sockaddr_in *)mapped;
                memset(s, 0, sizeof *s);
                s->sin_family = AF_INET;
                s->sin_port = htons(port);
                uint32_t xaddr;
                memcpy(&xaddr, v + 4, 4);
                s->sin_addr.s_addr = xaddr ^ htonl(MAGIC);
                return true;
            } else if (v[1] == 0x02 && alen >= 20) {  // IPv6
                auto *s = (struct sockaddr_in6 *)mapped;
                memset(s, 0, sizeof *s);
                s->sin6_family = AF_INET6;
                s->sin6_port = htons(port);
                uint8_t xaddr[16];
                memcpy(xaddr, v + 4, 16);
                uint32_t magic = htonl(MAGIC);
                for (int i = 0; i < 4; i++) xaddr[i] ^= ((uint8_t *)&magic)[i];
                for (int i = 0; i < 12; i++) xaddr[4 + i] ^= txid[i];
                memcpy(&s->sin6_addr, xaddr, 16);
                return true;
            }
        }
        off += 4 + ((alen + 3u) & ~3u);
    }
    return false;
}

} // namespace rivt::net
