// Membership op-log: chain verification, member derivation, and that
// forgery/tampering are rejected. Uses real P-256 identities on disk.
#include "test.h"
#include "net/identity.h"
#include "net/membership.h"
#include "net/rendezvous.h"

#include <cstdlib>
#include <sys/stat.h>

using namespace rivt;
using namespace rivt::net;

static std::string tmpd(const char *tag) {
    char buf[256];
    snprintf(buf, sizeof buf, "/tmp/rivt-mem-%s-XXXXXX", tag);
    return mkdtemp(buf);
}

TEST(membership_genesis_and_add) {
    auto a = Identity::load_or_create(tmpd("a"));
    auto b = Identity::load_or_create(tmpd("b"));
    ASSERT_TRUE(a && b);

    MembershipLog log;
    ASSERT_TRUE(!MembershipLog::genesis(*a).empty());
    ASSERT_TRUE(log.load({MembershipLog::genesis(*a)}));
    ASSERT_EQ((int)log.members().size(), 1);
    ASSERT_TRUE(log.is_member(a->spki_der()));
    ASSERT_FALSE(log.is_member(b->spki_der()));

    // A admits B.
    ASSERT_TRUE(!log.add_member(*a, b->spki_der(), "boxb", b->cert_pem()).empty());
    ASSERT_EQ((int)log.members().size(), 2);
    ASSERT_TRUE(log.is_member(b->spki_der()));

    // A fresh log verifies the same op list end to end.
    MembershipLog reload;
    ASSERT_TRUE(reload.load(log.ops()));
    ASSERT_EQ((int)reload.members().size(), 2);
}

TEST(membership_remove_and_transitive_add) {
    auto a = Identity::load_or_create(tmpd("a2"));
    auto b = Identity::load_or_create(tmpd("b2"));
    auto c = Identity::load_or_create(tmpd("c2"));
    MembershipLog log;
    log.load({MembershipLog::genesis(*a)});
    log.add_member(*a, b->spki_der(), "b", b->cert_pem());
    // B (a member) admits C — authority is transitive.
    ASSERT_TRUE(!log.add_member(*b, c->spki_der(), "c", c->cert_pem()).empty());
    ASSERT_EQ((int)log.members().size(), 3);
    // A removes B; C remains (B's earlier ops are not retroactively void).
    ASSERT_TRUE(!log.remove_member(*a, b->spki_der()).empty());
    ASSERT_FALSE(log.is_member(b->spki_der()));
    ASSERT_TRUE(log.is_member(c->spki_der()));
    ASSERT_EQ((int)log.members().size(), 2);

    MembershipLog reload;
    ASSERT_TRUE(reload.load(log.ops()));
    ASSERT_EQ((int)reload.members().size(), 2);
}

TEST(membership_expiry) {
    auto a = Identity::load_or_create(tmpd("a3"));
    auto b = Identity::load_or_create(tmpd("b3"));
    MembershipLog log;
    log.load({MembershipLog::genesis(*a)});
    log.add_member(*a, b->spki_der(), "b", b->cert_pem(), /*expires=*/1000);
    ASSERT_TRUE(log.is_member(b->spki_der(), 500));    // not yet expired
    ASSERT_FALSE(log.is_member(b->spki_der(), 2000));  // expired
    ASSERT_TRUE(log.is_member(b->spki_der(), 0));      // now=0 ignores expiry
}

TEST(membership_rejects_nonmember_signer) {
    auto a = Identity::load_or_create(tmpd("a4"));
    auto evil = Identity::load_or_create(tmpd("evil"));
    auto victim = Identity::load_or_create(tmpd("vic"));
    MembershipLog genesis_only;
    genesis_only.load({MembershipLog::genesis(*a)});

    // `evil` is not in a's set, so an add it signs must not verify when
    // spliced onto a's genesis.
    MembershipLog evil_log;
    evil_log.load({MembershipLog::genesis(*a)});
    // Forge: build an op from evil by giving evil its own single-op log
    // then trying to graft. Simplest: evil founds its own set and we try
    // to load [a-genesis, evil-add] — the evil add's signer isn't a's member.
    MembershipLog evil_set;
    evil_set.load({MembershipLog::genesis(*evil)});
    std::string evil_add = evil_set.add_member(*evil, victim->spki_der(), "v", victim->cert_pem());
    ASSERT_TRUE(!evil_add.empty());  // valid in evil's own set

    MembershipLog spliced;
    ASSERT_FALSE(spliced.load({MembershipLog::genesis(*a), evil_add}));  // seq/prev/signer all wrong
}

TEST(membership_rejects_tampering) {
    auto a = Identity::load_or_create(tmpd("a5"));
    auto b = Identity::load_or_create(tmpd("b5"));
    MembershipLog log;
    log.load({MembershipLog::genesis(*a)});
    log.add_member(*a, b->spki_der(), "b", b->cert_pem());
    auto ops = log.ops();

    // Flip a byte in the genesis op's signature region.
    auto bad = ops;
    bad[0][bad[0].size() - 1] ^= 0x01;
    MembershipLog t1;
    ASSERT_FALSE(t1.load(bad));

    // Reorder ops (add before genesis).
    MembershipLog t2;
    ASSERT_FALSE(t2.load({ops[1], ops[0]}));

    // Drop the genesis (log must start with genesis).
    MembershipLog t3;
    ASSERT_FALSE(t3.load({ops[1]}));

    // Truncate an op.
    auto trunc = ops;
    trunc[1].resize(trunc[1].size() / 2);
    MembershipLog t4;
    ASSERT_FALSE(t4.load(trunc));
}

TEST(membership_persist_and_bundle) {
    std::string dir = tmpd("persist");
    auto a = Identity::load_or_create(dir + "/a");
    auto b = Identity::load_or_create(dir + "/b");
    MembershipLog log;
    log.load({MembershipLog::genesis(*a)});
    log.add_member(*a, b->spki_der(), "boxb", b->cert_pem());

    std::string path = dir + "/membership.log";
    ASSERT_TRUE(log.save(path));
    ASSERT_TRUE(!log.set_id().empty());

    MembershipLog reloaded;
    ASSERT_TRUE(reloaded.load_file(path));
    ASSERT_EQ((int)reloaded.members().size(), 2);
    ASSERT_STR_EQ(reloaded.set_id(), log.set_id());  // stable id

    // Bundle = concatenated member cert PEMs; both certs present.
    std::string bundle = dir + "/authorized.pem";
    ASSERT_TRUE(log.write_bundle(bundle, 0));
    FILE *f = fopen(bundle.c_str(), "r");
    ASSERT_TRUE(f != nullptr);
    std::string content;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);
    ASSERT_TRUE(content.find(a->cert_pem()) != std::string::npos);
    ASSERT_TRUE(content.find(b->cert_pem()) != std::string::npos);

    // Expired members are excluded from the bundle.
    MembershipLog log2;
    log2.load({MembershipLog::genesis(*a)});
    log2.add_member(*a, b->spki_der(), "boxb", b->cert_pem(), 1000);
    std::string bundle2 = dir + "/authorized2.pem";
    ASSERT_TRUE(log2.write_bundle(bundle2, 2000));  // b expired
    content.clear();
    f = fopen(bundle2.c_str(), "r");
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) content.append(buf, n);
    fclose(f);
    ASSERT_TRUE(content.find(a->cert_pem()) != std::string::npos);
    ASSERT_TRUE(content.find(b->cert_pem()) == std::string::npos);
}

// Live DO round-trip: push a locally-built log op-by-op, fetch it back
// fresh, and verify the chain reconstructs. Skips without RIVT_RENDEZVOUS.
TEST(membership_do_sync) {
    const char *url = getenv("RIVT_RENDEZVOUS");
    if (!url) { fprintf(stderr, "  [do-sync] RIVT_RENDEZVOUS unset — skipping\n"); return; }
    std::string dir = tmpd("dosync");
    auto a = Identity::load_or_create(dir + "/a");
    auto b = Identity::load_or_create(dir + "/b");
    MembershipLog log;
    log.load({MembershipLog::genesis(*a)});
    log.add_member(*a, b->spki_der(), "boxb", b->cert_pem());
    std::string set = log.set_id();  // unique per test run (fresh keys)

    for (size_t i = 0; i < log.ops().size(); i++) {
        int rc = net::membership_push(url, set, (uint32_t)i, log.ops()[i]);
        if (rc == -1 && i == 0) {
            fprintf(stderr, "  [do-sync] push failed (offline?) — skipping\n");
            return;
        }
        ASSERT_EQ(rc, 0);
    }
    std::vector<std::string> fetched;
    ASSERT_TRUE(net::membership_fetch(url, set, fetched));
    ASSERT_EQ((int)fetched.size(), (int)log.ops().size());
    MembershipLog rebuilt;
    ASSERT_TRUE(rebuilt.load(fetched));  // full chain verification
    ASSERT_EQ((int)rebuilt.members().size(), 2);
    ASSERT_STR_EQ(rebuilt.set_id(), set);

    // seq conflict: re-pushing seq 0 must be rejected.
    ASSERT_EQ(net::membership_push(url, set, 0, log.ops()[0]), 1);
    fprintf(stderr, "  [do-sync] pushed/fetched/verified %d ops\n", (int)fetched.size());
}

int main() { return run_tests(); }
