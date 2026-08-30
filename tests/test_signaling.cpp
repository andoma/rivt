// Offer/answer candidate exchange between two Signaling peers through
// the live rendezvous DO. Skips without RIVT_RENDEZVOUS.
#include "test.h"
#include "core/event_loop.h"
#include "net/identity.h"
#include "net/signaling.h"

#include <cstdlib>
#include <functional>
#include <sys/stat.h>

using namespace rivt;
using namespace rivt::net;

static std::string tmpd(const char *t) {
    char b[256]; snprintf(b, sizeof b, "/tmp/rivt-sig-%s-XXXXXX", t); return mkdtemp(b);
}
static bool pump_until(EventLoop &l, std::function<bool()> p, int ms) {
    for (int i = 0; i < ms / 5; i++) { if (p()) return true; l.poll(5); }
    return p();
}

TEST(signaling_offer_answer) {
    const char *url = getenv("RIVT_RENDEZVOUS");
    if (!url) { fprintf(stderr, "  [sig] RIVT_RENDEZVOUS unset — skipping\n"); return; }
    auto a = Identity::load_or_create(tmpd("a"));
    auto b = Identity::load_or_create(tmpd("b"));

    EventLoop loop;
    Signaling sa(loop, *a), sb(loop, *b);
    bool a_ready = false, b_ready = false;
    sa.on_ready = [&] { a_ready = true; };
    sb.on_ready = [&] { b_ready = true; };
    ASSERT_TRUE(sa.start(url));
    ASSERT_TRUE(sb.start(url));
    if (!pump_until(loop, [&] { return a_ready && b_ready; }, 8000)) {
        fprintf(stderr, "  [sig] WS did not open (offline?) — skipping\n");
        return;
    }

    std::vector<Candidate> got;
    std::string got_from;
    bool got_answer = false, got_offer = false;
    sb.on_candidates = [&](const std::string &from, bool ans, std::vector<Candidate>) {
        if (!ans) { got_offer = true; sb.send(from, true, {{"203.0.113.9", 51111, "stun"}}); }
    };
    sa.on_candidates = [&](const std::string &from, bool ans, std::vector<Candidate> c) {
        got_from = from; got_answer = ans; got = std::move(c);
    };

    // A offers to B; B answers.
    sa.send(sb.my_id(), false, {{"10.0.0.5", 7433, "local"}, {"198.51.100.2", 40001, "stun"}});
    ASSERT_TRUE(pump_until(loop, [&] { return got_answer; }, 6000));
    ASSERT_TRUE(got_offer);
    ASSERT_EQ((int)got.size(), 1);
    ASSERT_STR_EQ(got[0].kind, "stun");
    ASSERT_STR_EQ(got[0].host, "203.0.113.9");
    ASSERT_EQ((int)got[0].port, 51111);
    ASSERT_STR_EQ(got_from, sb.my_id());
    fprintf(stderr, "  [sig] offer->answer roundtrip ok\n");
}

int main() { return run_tests(); }
