#include "net/ws_client.h"
#include "net/sock.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

namespace rivt::net {

static std::string base64(const uint8_t *d, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

WsClient::~WsClient() { close(); }

void WsClient::want(bool rd, bool wr) {
    uint32_t m = (rd ? EV_READ : 0) | (wr ? EV_WRITE : 0);
    m_armed_write = wr;
    m_loop.modify_fd(m_fd, m);
}

bool WsClient::connect(const std::string &url) {
    // Reset so a dead client can reconnect: drop any half-written or
    // half-parsed bytes from the previous transport.
    close();
    m_out.clear();
    m_in.clear();
    m_http.clear();
    m_armed_write = false;
    m_state = State::Idle;

    std::string u = url;
    if (u.rfind("wss://", 0) == 0) u = u.substr(6);
    else if (u.rfind("https://", 0) == 0) u = u.substr(8);
    auto slash = u.find('/');
    m_path = slash == std::string::npos ? "/" : u.substr(slash);
    std::string hostport = slash == std::string::npos ? u : u.substr(0, slash);
    std::string port = "443";
    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) { port = hostport.substr(colon + 1); m_host = hostport.substr(0, colon); }
    else m_host = hostport;

    struct addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(m_host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return false;
    m_fd = socket_cloexec(res->ai_family, SOCK_STREAM, 0, /*nonblock=*/true);
    if (m_fd < 0) { freeaddrinfo(res); return false; }
    int rc = ::connect(m_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { ::close(m_fd); m_fd = -1; return false; }

    m_ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_default_verify_paths(m_ctx);
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, nullptr);
    m_ssl = SSL_new(m_ctx);
    SSL_set_fd(m_ssl, m_fd);
    SSL_set_tlsext_host_name(m_ssl, m_host.c_str());
    SSL_set1_host(m_ssl, m_host.c_str());
    SSL_set_connect_state(m_ssl);

    uint8_t key[16];
    RAND_bytes(key, sizeof key);
    m_ws_key = base64(key, sizeof key);

    m_state = State::TcpConnect;
    m_loop.add_fd(m_fd, [this](uint32_t ev) { on_event(ev); }, EV_READ | EV_WRITE);
    return true;
}

void WsClient::close() {
    if (m_fd >= 0) { m_loop.remove_fd(m_fd); ::close(m_fd); m_fd = -1; }
    if (m_ssl) { SSL_free(m_ssl); m_ssl = nullptr; }
    if (m_ctx) { SSL_CTX_free(m_ctx); m_ctx = nullptr; }
    m_state = State::Dead;
}

void WsClient::fail() {
    if (m_state == State::Dead) return;
    close();
    if (on_close) on_close();
}

void WsClient::on_event(uint32_t events) {
    if (events & (EV_HUP | EV_ERR)) { fail(); return; }
    if (m_state == State::TcpConnect) m_state = State::TlsHandshake;
    if (m_state == State::TlsHandshake || m_state == State::Upgrade || m_state == State::Open)
        pump_tls();
}

void WsClient::pump_tls() {
    if (m_state == State::TlsHandshake) {
        int r = SSL_connect(m_ssl);
        if (r != 1) {
            int e = SSL_get_error(m_ssl, r);
            if (e == SSL_ERROR_WANT_READ) { want(true, false); return; }
            if (e == SSL_ERROR_WANT_WRITE) { want(true, true); return; }
            fail();
            return;
        }
        // Send the upgrade request.
        std::string req = "GET " + m_path + " HTTP/1.1\r\n"
                          "Host: " + m_host + "\r\n"
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + m_ws_key + "\r\n"
                          "Sec-WebSocket-Version: 13\r\n"
                          "User-Agent: rivt/1\r\n\r\n";
        m_out += req;
        m_state = State::Upgrade;
    }

    // Flush pending plaintext.
    while (!m_out.empty()) {
        int w = SSL_write(m_ssl, m_out.data(), (int)m_out.size());
        if (w > 0) { m_out.erase(0, w); continue; }
        int e = SSL_get_error(m_ssl, w);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) { want(true, true); return; }
        fail();
        return;
    }

    // Read whatever is available.
    for (;;) {
        char buf[16384];
        int r = SSL_read(m_ssl, buf, sizeof buf);
        if (r > 0) {
            if (m_state == State::Upgrade) m_http.append(buf, r);
            else m_in.append(buf, r);
            continue;
        }
        int e = SSL_get_error(m_ssl, r);
        if (e == SSL_ERROR_WANT_READ) break;
        if (e == SSL_ERROR_WANT_WRITE) { want(true, true); return; }
        if (e == SSL_ERROR_ZERO_RETURN) { fail(); return; }
        // WANT_READ with nothing pending is normal; anything else is fatal.
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        fail();
        return;
    }

    if (m_state == State::Upgrade) do_upgrade_read();
    if (m_state == State::Open) parse_frames();
    want(true, !m_out.empty());
}

void WsClient::do_upgrade_read() {
    auto end = m_http.find("\r\n\r\n");
    if (end == std::string::npos) return;
    // Verify Sec-WebSocket-Accept = base64(sha1(key + GUID)).
    std::string expect_src = m_ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha[SHA_DIGEST_LENGTH];
    SHA1((const uint8_t *)expect_src.data(), expect_src.size(), sha);
    std::string accept = base64(sha, sizeof sha);
    if (m_http.compare(0, 12, "HTTP/1.1 101") != 0 ||
        m_http.find(accept) == std::string::npos) {
        fail();
        return;
    }
    m_in = m_http.substr(end + 4);  // any frames already buffered
    m_http.clear();
    m_state = State::Open;
    if (on_open) on_open();
}

void WsClient::parse_frames() {
    size_t off = 0;
    while (m_in.size() - off >= 2) {
        const uint8_t *p = (const uint8_t *)m_in.data() + off;
        bool fin = p[0] & 0x80;
        int opcode = p[0] & 0x0f;
        bool masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7f;
        size_t hdr = 2;
        if (len == 126) { if (m_in.size() - off < 4) break; len = (p[2] << 8) | p[3]; hdr = 4; }
        else if (len == 127) {
            if (m_in.size() - off < 10) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
            hdr = 10;
        }
        size_t maskoff = hdr;
        if (masked) hdr += 4;
        if (m_in.size() - off < hdr + len) break;
        std::string payload(m_in.data() + off + hdr, len);
        if (masked)
            for (size_t i = 0; i < len; i++)
                payload[i] ^= m_in[off + maskoff + (i & 3)];
        off += hdr + len;
        (void)fin;

        if (opcode == 0x1 || opcode == 0x2) {           // text/binary
            if (on_message) on_message(payload);
        } else if (opcode == 0x8) {                     // close
            fail();
            return;
        } else if (opcode == 0x9) {                     // ping -> pong
            encode_frame(0xA, payload);
        }
    }
    if (off) m_in.erase(0, off);
}

void WsClient::encode_frame(int opcode, const std::string &payload) {
    std::string f;
    f += (char)(0x80 | opcode);
    size_t n = payload.size();
    uint8_t mask_flag = 0x80;  // client frames MUST be masked
    if (n < 126) f += (char)(mask_flag | n);
    else if (n < 65536) { f += (char)(mask_flag | 126); f += (char)(n >> 8); f += (char)(n & 0xff); }
    else {
        f += (char)(mask_flag | 127);
        for (int i = 7; i >= 0; i--) f += (char)((n >> (i * 8)) & 0xff);
    }
    uint8_t mask[4];
    RAND_bytes(mask, 4);
    f.append((char *)mask, 4);
    size_t base = f.size();
    f += payload;
    for (size_t i = 0; i < n; i++) f[base + i] ^= mask[i & 3];
    m_out += f;
    // Don't flush re-entrantly: send_text may be called from inside
    // pump_tls (via on_message). Just arm write; the event loop (or the
    // enclosing pump_tls's tail) flushes. Prevents unbounded recursion.
    if (m_state == State::Open) want(true, true);
}

void WsClient::send_text(const std::string &msg) {
    if (m_state != State::Open) return;
    encode_frame(0x1, msg);
}

} // namespace rivt::net
