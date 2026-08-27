#include "net/signaling.h"
#include "net/identity.h"

#include <openssl/sha.h>
#include <cstdio>

namespace rivt::net {

std::string sha256_hex(const std::string &data) {
    uint8_t h[SHA256_DIGEST_LENGTH];
    SHA256((const uint8_t *)data.data(), data.size(), h);
    static const char *t = "0123456789abcdef";
    std::string o;
    for (unsigned char c : std::string((char *)h, sizeof h)) { o += t[c >> 4]; o += t[c & 15]; }
    return o;
}

// Flat-JSON string field (values here are ids / base64, no escaping).
static std::string jget(const std::string &j, const std::string &key) {
    auto k = j.find("\"" + key + "\":\"");
    if (k == std::string::npos) return {};
    k += key.size() + 4;
    auto e = j.find('"', k);
    return e == std::string::npos ? std::string{} : j.substr(k, e - k);
}

Signaling::Signaling(EventLoop &loop, const Identity &self)
    : m_loop(loop), m_ws(loop) {
    m_id = sha256_hex(self.spki_der());
}

bool Signaling::start(const std::string &rendezvous_url) {
    std::string url = rendezvous_url;
    if (url.rfind("https://", 0) == 0) url = "wss://" + url.substr(8);
    else if (url.rfind("http://", 0) == 0) url = "ws://" + url.substr(7);
    url += "/ws?device=" + m_id;

    m_ws.on_open = [this]() {
        if (getenv("RIVT_SIG_DEBUG")) fprintf(stderr, "signaling: ws open (id %.16s)\n", m_id.c_str());
        for (auto &f : m_pending) m_ws.send_text(f);
        m_pending.clear();
        if (on_ready) on_ready();
    };
    m_ws.on_message = [this](const std::string &f) { handle(f); };
    return m_ws.connect(url);
}

// Payload (before base64): line 0 = "offer"/"answer", then one line per
// candidate "kind host port".
void Signaling::send(const std::string &to_id, bool answer,
                     const std::vector<Candidate> &cands) {
    std::string body = answer ? "answer\n" : "offer\n";
    for (const auto &c : cands)
        body += c.kind + " " + c.host + " " + std::to_string(c.port) + "\n";
    std::string frame = "{\"type\":\"send\",\"to\":\"" + to_id +
                        "\",\"payload\":\"" + b64_encode(body) + "\"}";
    // Queue until the WS handshake completes; sends before open are lost.
    if (m_ws.is_open()) m_ws.send_text(frame);
    else m_pending.push_back(frame);
}

void Signaling::keepalive() {
    if (m_ws.is_open()) m_ws.send_text("{\"type\":\"ping\"}");
}

void Signaling::handle(const std::string &frame) {
    if (jget(frame, "type") != "msg") return;  // ignore roster/joined/etc.
    std::string from = jget(frame, "from");
    std::string body = b64_decode(jget(frame, "payload"));
    if (from.empty() || body.empty()) return;

    std::vector<Candidate> cands;
    bool answer = false;
    size_t line = 0, p = 0;
    while (p < body.size()) {
        size_t nl = body.find('\n', p);
        if (nl == std::string::npos) nl = body.size();
        std::string ln = body.substr(p, nl - p);
        p = nl + 1;
        if (line++ == 0) { answer = (ln == "answer"); continue; }
        if (ln.empty()) continue;
        // "kind host port"
        auto s1 = ln.find(' '), s2 = ln.rfind(' ');
        if (s1 == std::string::npos || s2 == s1) continue;
        Candidate c;
        c.kind = ln.substr(0, s1);
        c.host = ln.substr(s1 + 1, s2 - s1 - 1);
        c.port = (uint16_t)atoi(ln.c_str() + s2 + 1);
        cands.push_back(std::move(c));
    }
    if (on_candidates) on_candidates(from, answer, std::move(cands));
}

} // namespace rivt::net
