// WSS client roundtrip against the deployed rendezvous DO. Network- and
// deployment-dependent: skips cleanly (passes) if unreachable so it
// never blocks offline builds. Requires RIVT_WS_URL to run.
#include "test.h"
#include "core/event_loop.h"
#include "net/ws_client.h"

#include <cstdlib>
#include <functional>

using namespace rivt;
using namespace rivt::net;

static bool pump_until(EventLoop &loop, std::function<bool()> pred, int ms) {
    for (int i = 0; i < ms / 5; i++) {
        if (pred()) return true;
        loop.poll(5);
    }
    return pred();
}

TEST(ws_ferry_roundtrip) {
    const char *base = getenv("RIVT_WS_URL");
    if (!base) {
        fprintf(stderr, "  [ws] RIVT_WS_URL unset — skipping\n");
        return;
    }
    std::string token = getenv("RIVT_WS_TOKEN") ? getenv("RIVT_WS_TOKEN") : "";
    std::string a_url = std::string(base) + "/ws?set=wstest&device=A&token=" + token;
    std::string b_url = std::string(base) + "/ws?set=wstest&device=B&token=" + token;

    EventLoop loop;
    WsClient a(loop), b(loop);
    bool a_open = false, b_open = false;
    std::string b_got;
    a.on_open = [&] { a_open = true; };
    b.on_open = [&] { b_open = true; };
    b.on_message = [&](const std::string &m) { if (m.find("\"type\":\"msg\"") != std::string::npos) b_got = m; };

    ASSERT_TRUE(a.connect(a_url));
    ASSERT_TRUE(b.connect(b_url));
    if (!pump_until(loop, [&] { return a_open && b_open; }, 8000)) {
        fprintf(stderr, "  [ws] did not open (offline/undeployed?) — skipping\n");
        return;
    }
    // Ferry A -> DO -> B (the spike DO's "send" verb).
    a.send_text("{\"type\":\"send\",\"to\":\"B\",\"payload\":\"ping123\",\"echo\":1}");
    ASSERT_TRUE(pump_until(loop, [&] { return !b_got.empty(); }, 5000));
    ASSERT_TRUE(b_got.find("ping123") != std::string::npos);
    fprintf(stderr, "  [ws] ferried A->B ok\n");
}

int main() { return run_tests(); }
