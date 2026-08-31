#include "net/rendezvous.h"
#include "core/debug.h"
#include "net/identity.h"

#include <openssl/ssl.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <ifaddrs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "net/sock.h"
#include <sys/stat.h>
#include <unistd.h>

namespace rivt::net {

std::string rendezvous_url() {
    if (const char *e = getenv("RIVT_RENDEZVOUS")) return e;
    const char *cfg = getenv("XDG_CONFIG_HOME");
    std::string path = cfg && *cfg ? std::string(cfg) + "/rivt/rendezvous"
                                   : std::string(getenv("HOME") ? getenv("HOME") : ".") +
                                         "/.config/rivt/rendezvous";
    FILE *f = fopen(path.c_str(), "re");
    if (!f) return {};
    char line[512] = {0};
    if (!fgets(line, sizeof line, f)) { fclose(f); return {}; }
    fclose(f);
    std::string url = line;
    while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == '/'))
        url.pop_back();
    return url;
}

bool set_rendezvous_url(const std::string &url) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    std::string dir = cfg && *cfg ? std::string(cfg) + "/rivt"
                                  : std::string(getenv("HOME") ? getenv("HOME") : ".") +
                                        "/.config/rivt";
    // mkdir -p the config dir.
    std::string acc;
    for (size_t i = 0; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty()) mkdir(acc.c_str(), 0755);
        }
        if (i < dir.size()) acc += dir[i];
    }
    FILE *f = fopen((dir + "/rendezvous").c_str(), "we");
    if (!f) return false;
    fprintf(f, "%s\n", url.c_str());
    fclose(f);
    return true;
}

// Minimal blocking HTTPS/1.1 request. host from url ("https://host[/...]").
// TCP connect with an explicit deadline (non-blocking connect + poll),
// returning a blocking fd with 10s send/recv timeouts. The only phase
// left to the OS is getaddrinfo, which the resolver bounds (never
// infinite). Everything else network-wide must carry a timeout: one
// unbounded BIO_read once froze rivtd's event loop permanently.
static int tcp_connect_bounded(const std::string &host, uint16_t port, int timeout_ms) {
    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        dbg("https: dns failed for %s", host.c_str());
        return -1;
    }
    int fd = socket_cloexec(res->ai_family, SOCK_STREAM, 0, /*nonblock=*/true);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) {
        dbg("https: connect to %s failed immediately (errno %d)", host.c_str(), errno);
        ::close(fd);
        return -1;
    }
    if (rc < 0) {
        struct pollfd pf = {fd, POLLOUT, 0};
        if (poll(&pf, 1, timeout_ms) <= 0) {
            dbg("https: connect to %s timed out (%d ms)", host.c_str(), timeout_ms);
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            dbg("https: connect to %s failed (so_error %d)", host.c_str(), err);
            ::close(fd);
            return -1;
        }
    }
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static bool https_request(const std::string &url, const std::string &path,
                          const std::string &method, const std::string &body,
                          std::string &response_body) {
    std::string host = url;
    if (host.rfind("https://", 0) == 0) host = host.substr(8);
    auto slash = host.find('/');
    if (slash != std::string::npos) host.resize(slash);

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed_ms = [t0]() {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    const char *fail = nullptr;

    int fd = tcp_connect_bounded(host, 443, 10000);
    if (fd < 0) {
        dbg("https: %s %s%s -> connect failed (%d ms)", method.c_str(),
            host.c_str(), path.c_str(), elapsed_ms());
        return false;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { ::close(fd); return false; }
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());

    bool ok = false;
    std::string out;
    if (SSL_connect(ssl) != 1) {
        fail = "tls handshake";
    } else {
        char req[1024];
        int n = snprintf(req, sizeof req,
                         "%s %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "User-Agent: rivt/1\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: close\r\n\r\n",
                         method.c_str(), path.c_str(), host.c_str(), body.size());
        if (n > 0 && SSL_write(ssl, req, n) == n &&
            (body.empty() || SSL_write(ssl, body.data(), (int)body.size()) == (int)body.size())) {
            char buf[4096];
            int r;
            while ((r = SSL_read(ssl, buf, sizeof buf)) > 0) out.append(buf, r);
            ok = true;
        } else {
            fail = "write";
        }
    }
    SSL_free(ssl);
    ::close(fd);
    SSL_CTX_free(ctx);
    dbg("https: %s %s%s -> %s (%d ms, %zu bytes)", method.c_str(), host.c_str(),
        path.c_str(), fail ? fail : "ok", elapsed_ms(), out.size());
    if (!ok) return false;

    // Split headers/body; tolerate chunked (single-chunk responses from
    // the worker are typical; strip chunk framing crudely).
    auto hdr_end = out.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false;
    // Accept any 2xx (the TURN mint returns 201 Created).
    bool status_ok = out.size() > 11 && out[9] == '2';
    std::string b = out.substr(hdr_end + 4);
    if (out.find("Transfer-Encoding: chunked") != std::string::npos ||
        out.find("transfer-encoding: chunked") != std::string::npos) {
        std::string un;
        size_t p = 0;
        while (p < b.size()) {
            size_t eol = b.find("\r\n", p);
            if (eol == std::string::npos) break;
            long len = strtol(b.c_str() + p, nullptr, 16);
            if (len <= 0) break;
            un.append(b, eol + 2, len);
            p = eol + 2 + len + 2;
        }
        b = un;
    }
    response_body = b;
    return status_ok;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64_encode(const std::string &in) {
    std::string o;
    const uint8_t *d = (const uint8_t *)in.data();
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < in.size() ? d[i + 1] << 8 : 0) |
                     (i + 2 < in.size() ? d[i + 2] : 0);
        o += B64[(v >> 18) & 63];
        o += B64[(v >> 12) & 63];
        o += i + 1 < in.size() ? B64[(v >> 6) & 63] : '=';
        o += i + 2 < in.size() ? B64[v & 63] : '=';
    }
    return o;
}
std::string b64_decode(const std::string &in) {
    int t[256];
    for (int i = 0; i < 256; i++) t[i] = -1;
    for (int i = 0; i < 64; i++) t[(uint8_t)B64[i]] = i;
    std::string o;
    uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || t[(uint8_t)c] < 0) continue;
        v = (v << 6) | t[(uint8_t)c];
        bits += 6;
        if (bits >= 8) { bits -= 8; o += (char)((v >> bits) & 0xff); }
    }
    return o;
}

static std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// Tiny extractors for the directory's flat JSON responses.
static std::string json_str(const std::string &j, const std::string &key) {
    auto k = j.find("\"" + key + "\":\"");
    if (k == std::string::npos) return {};
    k += key.size() + 4;
    auto e = j.find('"', k);
    return e == std::string::npos ? std::string{} : j.substr(k, e - k);
}

static int64_t json_num(const std::string &j, const std::string &key) {
    auto k = j.find("\"" + key + "\":");
    if (k == std::string::npos) return 0;
    return strtoll(j.c_str() + k + key.size() + 3, nullptr, 10);
}

// IPv4 only: we don't speak IPv6 on the wire (clients skip v6
// candidates; see resolve_v6 in quic_engine.cpp).
std::vector<std::string> local_addresses() {
    std::vector<std::string> v4;
    struct ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return {};
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET6_ADDRSTRLEN];
        auto *sa = (struct sockaddr_in *)i->ifa_addr;
        if (ntohl(sa->sin_addr.s_addr) >> 24 == 127) continue;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf)) v4.push_back(buf);
    }
    freeifaddrs(ifs);
    return v4;
}

bool register_device(const std::string &base_url, const Identity &id,
                     const std::string &name, uint16_t port) {
    int64_t ts = (int64_t)time(nullptr);
    char payload[512];
    snprintf(payload, sizeof payload, "%s|%s|%u|%lld", name.c_str(),
             id.fingerprint().c_str(), port, (long long)ts);
    std::string sig = id.sign_b64(payload);
    std::string spki = id.spki_b64();
    if (sig.empty() || spki.empty()) return false;

    std::string addrs;
    for (const auto &a : local_addresses()) {
        if (!addrs.empty()) addrs += ",";
        addrs += "\"" + json_escape(a) + "\"";
    }
    std::string body = "{\"name\":\"" + json_escape(name) +
                       "\",\"fingerprint\":\"" + id.fingerprint() +
                       "\",\"port\":" + std::to_string(port) +
                       ",\"spki\":\"" + spki + "\",\"sig\":\"" + sig +
                       "\",\"ts\":" + std::to_string(ts) +
                       ",\"addrs\":[" + addrs + "]}";
    std::string resp;
    if (!https_request(base_url, "/dir/register", "POST", body, resp)) {
        rivt::logmsg("rivtd: rendezvous register failed: %s\n", resp.c_str());
        return false;
    }
    return true;
}

bool lookup_device(const std::string &base_url, const std::string &name, DirEntry &out) {
    std::string resp;
    if (!https_request(base_url, "/dir/lookup?name=" + name, "GET", "", resp))
        return false;
    out.fingerprint = json_str(resp, "fingerprint");
    out.sig_id = json_str(resp, "sig_id");
    out.port = (uint16_t)json_num(resp, "port");
    out.last_seen_ms = json_num(resp, "last_seen");
    // addrs array = the peer's own interface addresses ("local").
    auto a = resp.find("\"addrs\":[");
    if (a != std::string::npos) {
        a += 9;
        auto end = resp.find(']', a);
        std::string list = resp.substr(a, end - a);
        size_t p = 0;
        while ((p = list.find('"', p)) != std::string::npos) {
            auto e = list.find('"', p + 1);
            if (e == std::string::npos) break;
            out.candidates.push_back({list.substr(p + 1, e - p - 1), out.port, "local"});
            p = e + 1;
        }
    }
    // The rendezvous-observed public IP (HTTP edge, not a STUN mapping).
    std::string obs = json_str(resp, "observed_ip");
    if (!obs.empty()) out.candidates.push_back({obs, out.port, "observed"});
    return out.port != 0;
}

int membership_push(const std::string &base_url, const std::string &set_id,
                    uint32_t seq, const std::string &op) {
    std::string body = "{\"set\":\"" + set_id + "\",\"seq\":" + std::to_string(seq) +
                       ",\"op\":\"" + b64_encode(op) + "\"}";
    std::string resp;
    bool ok = https_request(base_url, "/log/append", "POST", body, resp);
    if (ok) return 0;
    if (resp.find("seq conflict") != std::string::npos) return 1;
    return -1;
}

bool membership_fetch(const std::string &base_url, const std::string &set_id,
                      std::vector<std::string> &ops_out) {
    std::string resp;
    if (!https_request(base_url, "/log/fetch?set=" + set_id, "GET", "", resp)) return false;
    auto a = resp.find("\"ops\":[");
    if (a == std::string::npos) return false;
    a += 7;
    auto end = resp.find(']', a);
    std::string list = resp.substr(a, end - a);
    size_t p = 0;
    while ((p = list.find('"', p)) != std::string::npos) {
        auto e = list.find('"', p + 1);
        if (e == std::string::npos) break;
        ops_out.push_back(b64_decode(list.substr(p + 1, e - p - 1)));
        p = e + 1;
    }
    return true;
}

bool delete_device(const std::string &base_url, const Identity &self,
                   const std::string &name) {
    int64_t ts = (int64_t)time(nullptr);
    std::string payload = "delete|" + name + "|" + std::to_string(ts);
    std::string sig = self.sign_b64(payload), spki = self.spki_b64();
    if (sig.empty() || spki.empty()) return false;
    std::string body = "{\"name\":\"" + json_escape(name) + "\",\"spki\":\"" + spki +
                       "\",\"sig\":\"" + sig + "\",\"ts\":" + std::to_string(ts) + "}";
    std::string resp;
    return https_request(base_url, "/dir/delete", "POST", body, resp);
}

bool list_devices(const std::string &base_url, std::vector<RosterDevice> &out) {
    std::string resp;
    if (!https_request(base_url, "/dir/devices", "GET", "", resp)) return false;
    // Flat array of {name, fingerprint, last_seen}; walk by "name":" .
    size_t p = resp.find("\"devices\"");
    if (p == std::string::npos) return false;
    while ((p = resp.find("\"name\":\"", p)) != std::string::npos) {
        p += 8;
        size_t e = resp.find('"', p);
        if (e == std::string::npos) break;
        RosterDevice d;
        d.name = resp.substr(p, e - p);
        size_t ls = resp.find("\"last_seen\":", e);
        if (ls != std::string::npos) d.last_seen_ms = strtoll(resp.c_str() + ls + 12, nullptr, 10);
        out.push_back(std::move(d));
        p = e + 1;
    }
    return true;
}

bool turn_credentials(const std::string &base_url, std::string &user,
                      std::string &pass, std::string &host, uint16_t &port) {
    std::string resp;
    if (!https_request(base_url, "/turn/credentials", "GET", "", resp)) return false;
    user = json_str(resp, "username");
    pass = json_str(resp, "credential");
    // Pick the udp turn: url. urls is an array of strings.
    auto u = resp.find("turn:");
    while (u != std::string::npos) {
        auto e = resp.find('"', u);
        std::string url = resp.substr(u, e - u);
        if (url.find("transport=udp") != std::string::npos) {
            // turn:host:port?transport=udp
            auto h = url.substr(5);
            auto colon = h.find(':');
            auto q = h.find('?');
            host = h.substr(0, colon);
            port = (uint16_t)atoi(h.substr(colon + 1, q - colon - 1).c_str());
            break;
        }
        u = resp.find("turn:", u + 5);
    }
    return !user.empty() && !pass.empty() && !host.empty() && port != 0;
}

bool pair_put(const std::string &base_url, const std::string &invite_id,
              const std::string &box, const std::string &payload_b64) {
    std::string body = "{\"id\":\"" + invite_id + "\",\"box\":\"" + box +
                       "\",\"payload\":\"" + payload_b64 + "\"}";
    std::string resp;
    return https_request(base_url, "/pair/put", "POST", body, resp);
}

bool pair_get(const std::string &base_url, const std::string &invite_id,
              const std::string &box, std::string &payload_b64_out) {
    std::string resp;
    if (!https_request(base_url, "/pair/get?id=" + invite_id + "&box=" + box, "GET", "", resp))
        return false;
    if (resp.find("\"empty\"") != std::string::npos) return false;
    payload_b64_out = json_str(resp, "payload");
    return !payload_b64_out.empty();
}

} // namespace rivt::net
