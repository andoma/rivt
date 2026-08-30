// QUIC engine integration test: mutual-auth loopback connect, framed
// data exchange, and rejection of an unauthorized identity.
#include "test.h"
#include "core/event_loop.h"
#include "net/identity.h"
#include "net/quic_engine.h"
#include "net/membership.h"
#include "net/rendezvous.h"
#include "net/turn.h"
#include <arpa/inet.h>

#include <cstdlib>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rivt;
using namespace rivt::net;

static std::string tmpd(const char *tag) {
    std::string t = std::string("/tmp/rivt-quic-test-") + tag + "-XXXXXX";
    char buf[256];
    snprintf(buf, sizeof buf, "%s", t.c_str());
    return mkdtemp(buf);
}

static void append_file(const std::string &path, const std::string &content) {
    FILE *f = fopen(path.c_str(), "a");
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

// Drive the loop until pred() or timeout.
static bool pump_until(EventLoop &loop, std::function<bool()> pred, int ms = 5000) {
    for (int i = 0; i < ms / 5; i++) {
        if (pred()) return true;
        loop.poll(5);
    }
    return pred();
}

TEST(quic_mutual_auth_roundtrip) {
    std::string da = tmpd("a"), db = tmpd("b");
    auto ida = Identity::load_or_create(da);
    auto idb = Identity::load_or_create(db);
    ASSERT_TRUE(ida && idb);
    ASSERT_TRUE(ida->fingerprint() != idb->fingerprint());

    // Cross-authorize: each side's bundle holds the other's cert.
    std::string bundle_a = da + "/authorized.pem";
    std::string bundle_b = db + "/authorized.pem";
    append_file(bundle_a, idb->cert_pem());
    append_file(bundle_b, ida->cert_pem());

    EventLoop loop;
    auto server = QuicEngine::listen(loop, 0, *ida, bundle_a);
    ASSERT_TRUE(server != nullptr);
    // Grab the bound port via a second engine? Simpler: fixed test port.
    server.reset();
    uint16_t port = 47831;
    server = QuicEngine::listen(loop, port, *ida, bundle_a);
    ASSERT_TRUE(server != nullptr);

    std::string got_at_server, got_at_client;
    QuicEngine::Conn *server_side = nullptr;
    server->on_data = [&](QuicEngine::Conn *c, uint64_t, const uint8_t *d, size_t n) {
        server_side = c;
        got_at_server.append((const char *)d, n);
    };

    auto client = QuicEngine::connect(loop, "127.0.0.1", port, *idb, bundle_b);
    ASSERT_TRUE(client != nullptr);
    bool connected = false;
    client->on_connected = [&](QuicEngine::Conn *) { connected = true; };
    client->on_data = [&](QuicEngine::Conn *, uint64_t, const uint8_t *d, size_t n) {
        got_at_client.append((const char *)d, n);
    };

    ASSERT_TRUE(pump_until(loop, [&] { return connected; }));
    client->send(client->client_conn(), 0, "hello-over-quic", 15);
    ASSERT_TRUE(pump_until(loop, [&] { return got_at_server.size() >= 15; }));
    ASSERT_STR_EQ(got_at_server, "hello-over-quic");

    server->send(server_side, 0, "welcome", 7);
    ASSERT_TRUE(pump_until(loop, [&] { return got_at_client.size() >= 7; }));
    ASSERT_STR_EQ(got_at_client, "welcome");
}

TEST(quic_rejects_unauthorized_peer) {
    std::string da = tmpd("s"), dc = tmpd("x");
    auto ida = Identity::load_or_create(da);
    auto idx = Identity::load_or_create(dc);  // NOT in the server's bundle
    std::string bundle_a = da + "/authorized.pem";
    append_file(bundle_a, "");  // empty bundle: trust nobody
    std::string bundle_x = dc + "/authorized.pem";
    append_file(bundle_x, ida->cert_pem());  // client trusts server

    EventLoop loop;
    uint16_t port = 47832;
    auto server = QuicEngine::listen(loop, port, *ida, bundle_a);
    ASSERT_TRUE(server != nullptr);
    std::string leaked;
    server->on_data = [&](QuicEngine::Conn *, uint64_t, const uint8_t *d, size_t n) {
        leaked.append((const char *)d, n);
    };

    auto client = QuicEngine::connect(loop, "127.0.0.1", port, *idx, bundle_x);
    ASSERT_TRUE(client != nullptr);
    bool connected = false, closed = false;
    client->on_connected = [&](QuicEngine::Conn *) { connected = true; };
    client->on_closed = [&](QuicEngine::Conn *) { closed = true; };
    client->send(client->client_conn(), 0, "secret", 6);

    pump_until(loop, [&] { return closed; }, 4000);
    ASSERT_TRUE(!connected || closed);  // handshake must not complete usably
    ASSERT_STR_EQ(leaked, "");          // and no data may cross
}

TEST(quic_bulk_throughput_with_backpressure) {
    std::string da = tmpd("t1"), db = tmpd("t2");
    auto ida = Identity::load_or_create(da);
    auto idb = Identity::load_or_create(db);
    std::string bundle_a = da + "/authorized.pem", bundle_b = db + "/authorized.pem";
    append_file(bundle_a, idb->cert_pem());
    append_file(bundle_b, ida->cert_pem());

    EventLoop loop;
    uint16_t port = 47833;
    auto server = QuicEngine::listen(loop, port, *ida, bundle_a);
    QuicEngine::Conn *sconn = nullptr;
    size_t received = 0;
    uint8_t expect = 0;
    bool corrupt = false;
    server->on_data = [&](QuicEngine::Conn *c, uint64_t, const uint8_t *d, size_t n) {
        sconn = c;
        for (size_t i = 0; i < n; i++)
            if (d[i] != (uint8_t)(received + i)) corrupt = true;
        received += n;
        (void)expect;
    };

    auto client = QuicEngine::connect(loop, "127.0.0.1", port, *idb, bundle_b);
    bool connected = false;
    client->on_connected = [&](QuicEngine::Conn *) { connected = true; };
    ASSERT_TRUE(pump_until(loop, [&] { return connected; }));

    // Producer honoring the watermarks, as the daemon does with PTYs.
    constexpr size_t TOTAL = 20 << 20;
    size_t produced = 0;
    bool paused = false;
    client->on_drained = [&](QuicEngine::Conn *) { paused = false; };
    auto produce = [&]() {
        while (!paused && produced < TOTAL) {
            uint8_t chunk[65536];
            size_t n = TOTAL - produced < sizeof chunk ? TOTAL - produced : sizeof chunk;
            for (size_t i = 0; i < n; i++) chunk[i] = (uint8_t)(produced + i);
            client->send(client->client_conn(), 0, chunk, n);
            produced += n;
            if (client->client_conn()->queued() > QuicEngine::SEND_HIGH_WATER)
                paused = true;
        }
    };

    bool ok = pump_until(loop, [&] {
        produce();
        return received >= TOTAL;
    }, 30000);
    ASSERT_TRUE(ok);
    ASSERT_EQ(received, TOTAL);
    ASSERT_FALSE(corrupt);
    ASSERT_TRUE(sconn != nullptr);
}

// Network-dependent: hits real Cloudflare STUN. Skips (passes) if the
// socket can't reach it, so offline/sandboxed CI stays green.
TEST(quic_stun_reflexive) {
    std::string d = tmpd("stun");
    auto id = Identity::load_or_create(d);
    std::string bundle = d + "/authorized.pem";
    append_file(bundle, id->cert_pem());

    EventLoop loop;
    auto e = QuicEngine::listen(loop, 0, *id, bundle);
    ASSERT_TRUE(e != nullptr);
    ASSERT_TRUE(e->local_port() != 0);

    bool done = false, ok = false;
    char ip[64] = {0};
    uint16_t port = 0;
    e->discover_reflexive([&](bool good, const struct sockaddr_storage &sa) {
        done = true;
        ok = good;
        if (good) {
            if (sa.ss_family == AF_INET) {
                auto *s = (const struct sockaddr_in *)&sa;
                inet_ntop(AF_INET, &s->sin_addr, ip, sizeof ip);
                port = ntohs(s->sin_port);
            } else {
                auto *s = (const struct sockaddr_in6 *)&sa;
                inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof ip);
                port = ntohs(s->sin6_port);
            }
        }
    });
    pump_until(loop, [&] { return done; }, 6000);
    if (!ok) {
        fprintf(stderr, "  [stun] no response (offline?) — skipping\n");
        return;
    }
    fprintf(stderr, "  [stun] reflexive %s:%u\n", ip, port);
    ASSERT_TRUE(port != 0);
}

// The payoff: a membership log derives the QUIC trust bundle, and that
// bundle grants/denies connections. No hand-maintained cert lists.
TEST(quic_trust_derived_from_membership_log) {
    std::string da = tmpd("mla"), db = tmpd("mlb"), dc = tmpd("mlc");
    auto a = Identity::load_or_create(da);   // founder
    auto b = Identity::load_or_create(db);   // member
    auto c = Identity::load_or_create(dc);   // outsider

    MembershipLog log;
    log.load({MembershipLog::genesis(*a)});
    log.add_member(*a, b->spki_der(), "b", b->cert_pem());

    std::string bundle_a = da + "/bundle.pem", bundle_b = db + "/bundle.pem",
                bundle_c = dc + "/bundle.pem";
    ASSERT_TRUE(log.write_bundle(bundle_a, 0));
    ASSERT_TRUE(log.write_bundle(bundle_b, 0));
    ASSERT_TRUE(log.write_bundle(bundle_c, 0));  // c has the bundle but isn't in it

    EventLoop loop;
    uint16_t port = 47834;
    auto server = QuicEngine::listen(loop, port, *a, bundle_a);
    ASSERT_TRUE(server != nullptr);
    bool got = false;
    server->on_data = [&](QuicEngine::Conn *, uint64_t, const uint8_t *, size_t) { got = true; };

    // Member B (in A's log) connects — trusted.
    auto cb = QuicEngine::connect(loop, "127.0.0.1", port, *b, bundle_b);
    bool b_up = false;
    cb->on_connected = [&](QuicEngine::Conn *) { b_up = true; };
    ASSERT_TRUE(pump_until(loop, [&] { return b_up; }));
    cb->send(cb->client_conn(), 0, "hi", 2);
    ASSERT_TRUE(pump_until(loop, [&] { return got; }));

    // Outsider C (not in the log, so absent from A's bundle) is rejected.
    auto cc = QuicEngine::connect(loop, "127.0.0.1", port, *c, bundle_c);
    bool c_up = false, c_closed = false;
    cc->on_connected = [&](QuicEngine::Conn *) { c_up = true; };
    cc->on_closed = [&](QuicEngine::Conn *) { c_closed = true; };
    pump_until(loop, [&] { return c_closed; }, 4000);
    ASSERT_TRUE(!c_up || c_closed);
}

// Full relayed path through real Cloudflare TURN: the listener
// allocates a relay and the client connects only to that public
// relayed address (never a direct path). Proves TURN fallback works
// from a single network. Skips without RIVT_RENDEZVOUS.
TEST(quic_over_turn_relay) {
    const char *rdv = getenv("RIVT_RENDEZVOUS");
    if (!rdv) { fprintf(stderr, "  [turn] RIVT_RENDEZVOUS unset — skipping\n"); return; }

    std::string da = tmpd("ta"), db = tmpd("tb");
    auto a = Identity::load_or_create(da);   // listener
    auto b = Identity::load_or_create(db);   // client
    std::string bundle_a = da + "/b.pem", bundle_b = db + "/b.pem";
    append_file(bundle_a, b->cert_pem());
    append_file(bundle_b, a->cert_pem());

    std::string user, pass, thost; uint16_t tport;
    if (!net::turn_credentials(rdv, user, pass, thost, tport)) {
        fprintf(stderr, "  [turn] no creds (offline?) — skipping\n"); return;
    }

    EventLoop loop;
    // Listener with a TURN relay.
    auto server = QuicEngine::listen(loop, 0, *a, bundle_a);
    ASSERT_TRUE(server != nullptr);
    net::TurnRelay relay(loop);
    if (!relay.allocate(thost, tport, user, pass)) {
        fprintf(stderr, "  [turn] allocate failed — skipping\n"); return;
    }
    server->add_turn(&relay);
    std::string got_at_server;
    server->on_data = [&](QuicEngine::Conn *, uint64_t, const uint8_t *d, size_t n) {
        got_at_server.append((const char *)d, n);
    };

    // Client: a STUNable socket; we permit its reflexive on the relay,
    // then it connects to the relayed public address.
    auto client = QuicEngine::create_client(loop, *b, bundle_b);
    ASSERT_TRUE(client != nullptr);
    bool have_refl = false;
    struct sockaddr_in refl {};
    client->discover_reflexive([&](bool ok, const struct sockaddr_storage &sa) {
        if (ok) {
            // v4-mapped v6 or v4 — extract v4.
            if (sa.ss_family == AF_INET) refl = *(const struct sockaddr_in *)&sa;
            else {
                auto *s6 = (const struct sockaddr_in6 *)&sa;
                refl.sin_family = AF_INET;
                refl.sin_port = s6->sin6_port;
                memcpy(&refl.sin_addr, s6->sin6_addr.s6_addr + 12, 4);  // v4-mapped tail
            }
            have_refl = true;
        }
    });
    ASSERT_TRUE(pump_until(loop, [&] { return have_refl; }, 6000));
    relay.permit(refl);

    bool connected = false;
    client->on_connected = [&](QuicEngine::Conn *) { connected = true; };
    ASSERT_TRUE(client->start_connection(relay.relayed_host(), relay.relayed_port()));
    ASSERT_TRUE(pump_until(loop, [&] { return connected; }, 10000));
    client->send(client->client_conn(), 0, "via-turn-relay", 14);
    ASSERT_TRUE(pump_until(loop, [&] { return got_at_server.size() >= 14; }, 8000));
    ASSERT_STR_EQ(got_at_server, "via-turn-relay");
    fprintf(stderr, "  [turn] QUIC handshake + data over Cloudflare relay ok\n");
}

int main() { return run_tests(); }
