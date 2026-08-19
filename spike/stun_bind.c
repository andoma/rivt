// Minimal STUN Binding client (RFC 5389 subset).
// Usage: stun_bind [host] [port]   (default stun.cloudflare.com 3478)
// Prints the reflexive address:port and RTT. Exit 0 on success.
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STUN_MAGIC 0x2112A442u

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "stun.cloudflare.com";
    const char *port = argc > 2 ? argv[2] : "3478";

    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;  // v4 first; reflexive v4 is what NAT traversal needs
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) { fprintf(stderr, "resolve %s: %s\n", host, gai_strerror(rc)); return 1; }

    int fd = socket(res->ai_family, SOCK_DGRAM, 0);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    uint8_t req[20] = {0};
    req[0] = 0x00; req[1] = 0x01;              // Binding Request
    req[2] = 0x00; req[3] = 0x00;              // length 0
    uint32_t magic = htonl(STUN_MAGIC);
    memcpy(req + 4, &magic, 4);
    if (getrandom(req + 8, 12, 0) != 12) { perror("getrandom"); return 1; }

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (sendto(fd, req, sizeof req, 0, res->ai_addr, res->ai_addrlen) < 0) {
            perror("sendto"); return 1;
        }
        uint8_t buf[512];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  // retransmit
            perror("recv"); return 1;
        }
        gettimeofday(&t1, NULL);
        if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01 ||
            memcmp(buf + 8, req + 8, 12) != 0) {
            fprintf(stderr, "unexpected response (%zd bytes, type %02x%02x)\n", n, buf[0], buf[1]);
            return 1;
        }
        // Walk attributes for XOR-MAPPED-ADDRESS (0x0020)
        size_t off = 20, mlen = 20 + ((size_t)buf[2] << 8 | buf[3]);
        if (mlen > (size_t)n) mlen = n;
        while (off + 4 <= mlen) {
            uint16_t type = (uint16_t)(buf[off] << 8 | buf[off + 1]);
            uint16_t alen = (uint16_t)(buf[off + 2] << 8 | buf[off + 3]);
            if (type == 0x0020 && alen >= 8 && buf[off + 5] == 0x01) {  // IPv4
                uint16_t xport = (uint16_t)(buf[off + 6] << 8 | buf[off + 7]);
                uint32_t xaddr;
                memcpy(&xaddr, buf + off + 8, 4);
                uint32_t addr = ntohl(xaddr) ^ STUN_MAGIC;
                double rtt = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_usec - t0.tv_usec) / 1e3;
                printf("%u.%u.%u.%u:%u rtt=%.1fms\n",
                       addr >> 24, (addr >> 16) & 255, (addr >> 8) & 255, addr & 255,
                       xport ^ (uint16_t)(STUN_MAGIC >> 16), rtt);
                return 0;
            }
            off += 4 + ((alen + 3u) & ~3u);
        }
        fprintf(stderr, "no XOR-MAPPED-ADDRESS in response\n");
        return 1;
    }
    fprintf(stderr, "timeout after 3 attempts\n");
    return 1;
}
