// QUIC engine integration test: mutual-auth loopback connect, framed
// data exchange, and rejection of an unauthorized identity.
#include "test.h"
#include "core/event_loop.h"
#include "net/identity.h"
#include "net/quic_engine.h"

#include <cstdlib>
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
    server->on_data = [&](QuicEngine::Conn *c, const uint8_t *d, size_t n) {
        server_side = c;
        got_at_server.append((const char *)d, n);
    };

    auto client = QuicEngine::connect(loop, "127.0.0.1", port, *idb, bundle_b);
    ASSERT_TRUE(client != nullptr);
    bool connected = false;
    client->on_connected = [&](QuicEngine::Conn *) { connected = true; };
    client->on_data = [&](QuicEngine::Conn *, const uint8_t *d, size_t n) {
        got_at_client.append((const char *)d, n);
    };

    ASSERT_TRUE(pump_until(loop, [&] { return connected; }));
    client->send(client->client_conn(), "hello-over-quic", 15);
    ASSERT_TRUE(pump_until(loop, [&] { return got_at_server.size() >= 15; }));
    ASSERT_STR_EQ(got_at_server, "hello-over-quic");

    server->send(server_side, "welcome", 7);
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
    server->on_data = [&](QuicEngine::Conn *, const uint8_t *d, size_t n) {
        leaked.append((const char *)d, n);
    };

    auto client = QuicEngine::connect(loop, "127.0.0.1", port, *idx, bundle_x);
    ASSERT_TRUE(client != nullptr);
    bool connected = false, closed = false;
    client->on_connected = [&](QuicEngine::Conn *) { connected = true; };
    client->on_closed = [&](QuicEngine::Conn *) { closed = true; };
    client->send(client->client_conn(), "secret", 6);

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
    server->on_data = [&](QuicEngine::Conn *c, const uint8_t *d, size_t n) {
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
            client->send(client->client_conn(), chunk, n);
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

int main() { return run_tests(); }
