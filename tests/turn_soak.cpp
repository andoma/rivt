// turn_soak — standalone TURN diagnostic / soak tester.
//
// Allocates a relay via the configured rendezvous (like rivtd does),
// permits our own reflexive address, then plays both ends: a plain UDP
// "peer" socket sends a sequence number to the relayed address every
// second; the allocation owner echoes it back through a Send
// indication. This exercises the DATA path end to end — a zombie
// allocation keeps answering Refresh/CreatePermission with success
// while forwarding nothing, so control-plane checks cannot detect it.
//
// Run it for hours; it logs a line when the data path dies or recovers,
// alongside the control-plane state, and a status line every minute.
#include "core/debug.h"
#include "core/event_loop.h"
#include "net/rendezvous.h"
#include "net/sock.h"
#include "net/stun.h"
#include "net/turn.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <string>

using namespace rivt;
using steady = std::chrono::steady_clock;

static double secs_since(steady::time_point t) {
    return std::chrono::duration<double>(steady::now() - t).count();
}

// Blocking reflexive discovery on fd (already non-blocking; poll waits).
static bool discover_reflexive(int fd, struct sockaddr_in *out) {
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo("stun.cloudflare.com", "3478", &hints, &res) != 0 || !res)
        return false;
    struct sockaddr_in stun_srv {};
    memcpy(&stun_srv, res->ai_addr, sizeof stun_srv);
    freeaddrinfo(res);

    uint8_t txid[net::STUN_TXID_LEN];
    uint8_t req[20];
    net::stun_build_request(req, txid);
    for (int attempt = 0; attempt < 5; attempt++) {
        sendto(fd, req, sizeof req, 0, (struct sockaddr *)&stun_srv, sizeof stun_srv);
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 1000) <= 0) continue;
        uint8_t buf[512];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        struct sockaddr_storage mapped {};
        if (n > 0 && net::stun_parse_response(buf, (size_t)n, txid, &mapped) &&
            mapped.ss_family == AF_INET) {
            memcpy(out, &mapped, sizeof *out);
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    // Modes: default = continuous echo every 1s.
    // "idle [interval]" = allocate + permit, stay silent, then every
    // `interval` seconds (default 300) send a 3-packet probe burst and
    // report whether the data path still works — mimics a rivtd relay
    // sitting idle overnight between punches.
    bool idle_mode = argc > 1 && std::string(argv[1]) == "idle";
    int idle_interval = argc > 2 ? atoi(argv[2]) : 300;
    debug_enabled() = true;  // diagnostic tool: always show dbg()
    std::string rdv = net::rendezvous_url();
    if (rdv.empty()) { logmsg("turn-soak: no rendezvous configured\n"); return 1; }
    std::string user, pass, host;
    uint16_t port = 0;
    if (!net::turn_credentials(rdv, user, pass, host, port)) {
        logmsg("turn-soak: turn_credentials failed\n");
        return 1;
    }
    logmsg("turn-soak: creds ok (turn server %s:%u, user %.16s...)\n",
           host.c_str(), port, user.c_str());

    EventLoop loop;
    net::TurnRelay relay(loop);
    if (!relay.allocate(host, port, user, pass)) {
        logmsg("turn-soak: allocate failed\n");
        return 1;
    }

    int pfd = net::socket_cloexec(AF_INET, SOCK_DGRAM, 0, /*nonblock=*/true);
    if (pfd < 0) { logmsg("turn-soak: peer socket failed\n"); return 1; }
    struct sockaddr_in peer_pub {};
    if (!discover_reflexive(pfd, &peer_pub)) {
        logmsg("turn-soak: peer STUN failed\n");
        return 1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_pub.sin_addr, ip, sizeof ip);
    logmsg("turn-soak: peer reflexive %s:%u\n", ip, ntohs(peer_pub.sin_port));
    relay.permit(peer_pub);

    struct sockaddr_in relayed {};
    relayed.sin_family = AF_INET;
    relayed.sin_port = htons(relay.relayed_port());
    inet_pton(AF_INET, relay.relayed_host().c_str(), &relayed.sin_addr);

    uint64_t seq = 0, echoed = 0, inbound = 0;
    auto last_echo = steady::now();
    auto died_at = steady::now();
    bool dead = false;

    // Owner side: whatever arrives via the relay goes straight back out.
    relay.on_data = [&](const struct sockaddr_in &from, const uint8_t *d, size_t n) {
        inbound++;
        relay.send_to(from, d, n);
    };

    // Peer side: count echoes.
    loop.add_fd(pfd, [&](uint32_t ev) {
        if (!(ev & EV_READ)) return;
        uint8_t buf[512];
        for (;;) {
            ssize_t n = recv(pfd, buf, sizeof buf, 0);
            if (n <= 0) break;
            if (net::is_stun(buf, (size_t)n)) continue;
            echoed++;
            last_echo = steady::now();
            if (dead) {
                logmsg("turn-soak: DATA PATH RECOVERED after %.0fs (control alive=%d)\n",
                       secs_since(died_at), relay.alive());
                dead = false;
            }
        }
    });

    auto probe_window_echoes = std::make_shared<uint64_t>(0);
    loop.add_timer(1000, [&, probe_window_echoes]() {
        uint64_t tick = ++seq;
        if (idle_mode) {
            int phase = (int)(tick % (uint64_t)idle_interval);
            if (phase == 1) *probe_window_echoes = echoed;  // window opens
            if (phase >= 1 && phase <= 3) {
                char msg[64];
                int n = snprintf(msg, sizeof msg, "probe %llu", (unsigned long long)tick);
                sendto(pfd, msg, n, 0, (struct sockaddr *)&relayed, sizeof relayed);
            }
            if (phase == 8) {
                uint64_t got = echoed - *probe_window_echoes;
                logmsg("turn-soak: idle probe after %ds silence: %s "
                       "(%llu/3 echoed, control alive=%d)\n",
                       idle_interval - 8,
                       got ? "DATA PATH OK" : "DATA PATH DEAD",
                       (unsigned long long)got, relay.alive());
            }
            return;
        }
        char msg[64];
        int n = snprintf(msg, sizeof msg, "seq %llu", (unsigned long long)tick);
        sendto(pfd, msg, n, 0, (struct sockaddr *)&relayed, sizeof relayed);
        if (!dead && secs_since(last_echo) > 5.0) {
            dead = true;
            died_at = steady::now();
            logmsg("turn-soak: DATA PATH DEAD (no echo for %.1fs; control alive=%d, "
                   "owner-side inbound so far %llu)\n",
                   secs_since(last_echo), relay.alive(), (unsigned long long)inbound);
        }
    }, true);

    loop.add_timer(60000, [&]() {
        logmsg("turn-soak: status: sent %llu, echoed %llu, owner inbound %llu, "
               "last echo %.1fs ago, control alive=%d\n",
               (unsigned long long)seq, (unsigned long long)echoed,
               (unsigned long long)inbound, secs_since(last_echo), relay.alive());
    }, true);

    while (loop.poll(-1)) {}
    return 0;
}
