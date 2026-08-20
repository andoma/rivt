#include "remote/remote_client.h"
#include "core/debug.h"
#include "proto/frame.h"
#include "proto/messages.h"

#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rivt {

using proto::MsgType;

std::string RemoteClient::default_socket_path() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    std::string dir = rt ? std::string(rt) + "/rivt"
                         : "/tmp/rivt-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700);
    return dir + "/daemon.sock";
}

static int try_connect(const std::string &path) {
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return -1;
    strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Spawn rivtd detached (double fork so it isn't our child to reap).
// Looks for rivtd next to our own binary first, then falls back to PATH.
static void spawn_daemon(const std::string &socket_path) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild != 0) _exit(0);
        setsid();

        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            std::string sibling = std::string(dirname(self)) + "/rivtd";
            execl(sibling.c_str(), "rivtd", "--socket", socket_path.c_str(), (char *)nullptr);
        }
        execlp("rivtd", "rivtd", "--socket", socket_path.c_str(), (char *)nullptr);
        _exit(127);
    }
    waitpid(pid, nullptr, 0);  // reap the intermediate child
}

bool RemoteClient::connect(const std::string &path, bool autostart) {
    m_fd = try_connect(path);
    if (m_fd < 0 && autostart) {
        dbg("remote: no daemon at %s, spawning rivtd", path.c_str());
        spawn_daemon(path);
        for (int i = 0; i < 40 && m_fd < 0; i++) {
            usleep(50000);
            m_fd = try_connect(path);
        }
    }
    if (m_fd < 0) return false;

    fcntl(m_fd, F_SETFL, O_NONBLOCK);
    m_loop.add_fd(m_fd, [this](uint32_t ev) { on_event(ev); });
    return true;
}

void RemoteClient::close() {
    if (m_fd < 0) return;
    m_loop.remove_fd(m_fd);
    ::close(m_fd);
    m_fd = -1;
    m_in.clear();
    m_out.clear();
    m_out_off = 0;
    m_write_armed = false;
}

void RemoteClient::fail() {
    if (m_failing || m_fd < 0) return;
    m_failing = true;
    close();
    if (on_disconnect) on_disconnect();
    m_failing = false;
}

void RemoteClient::on_event(uint32_t ev) {
    if (ev & (EV_HUP | EV_ERR)) { fail(); return; }
    if (ev & EV_WRITE) flush();
    if (ev & EV_READ) {
        char buf[65536];
        for (;;) {
            ssize_t n = recv(m_fd, buf, sizeof buf, 0);
            if (n > 0) { m_in.append(buf, n); continue; }
            if (n == 0) { fail(); return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fail();
            return;
        }
        process();
    }
}

void RemoteClient::process() {
    while (m_fd >= 0 && m_in.size() >= proto::FRAME_HEADER_SIZE) {
        proto::FrameHeader h;
        if (!proto::decode_frame_header((const uint8_t *)m_in.data(), h)) {
            fail();
            return;
        }
        size_t total = proto::FRAME_HEADER_SIZE + h.len;
        if (m_in.size() < total) return;
        const uint8_t *payload = (const uint8_t *)m_in.data() + proto::FRAME_HEADER_SIZE;

        if (h.channel == 0) {
            dispatch_control(h.type, payload, h.len);
        } else if (h.type == proto::PANE_OUT) {
            if (on_output) on_output(h.channel, (const char *)payload, h.len);
        } else if (h.type == proto::PANE_SNAPSHOT) {
            if (on_snapshot) on_snapshot(h.channel, payload, h.len);
        }
        m_in.erase(0, total);
    }
}

void RemoteClient::dispatch_control(uint16_t type, const uint8_t *data, size_t len) {
    proto::Reader r(data, len);
    switch ((MsgType)type) {
    case MsgType::HelloOk:
        if (r.u32() == proto::PROTO_VERSION && r.ok) {
            if (on_hello_ok) on_hello_ok();
        } else {
            fail();
        }
        break;
    case MsgType::SessionList: {
        uint32_t n = r.u32();
        std::vector<RemoteSessionInfo> list;
        for (uint32_t i = 0; i < n && r.ok; i++) {
            RemoteSessionInfo s;
            s.id = r.u32();
            s.name = r.str();
            s.npanes = r.u32();
            list.push_back(std::move(s));
        }
        if (r.ok && on_session_list) on_session_list(list);
        break;
    }
    case MsgType::SessionCreated: {
        uint32_t sid = r.u32(), pane = r.u32();
        std::string err = r.str();
        if (r.ok && on_session_created) on_session_created(sid, pane, err);
        break;
    }
    case MsgType::AttachOk: {
        uint32_t sid = r.u32(), n = r.u32();
        std::vector<RemotePaneInfo> panes;
        for (uint32_t i = 0; i < n && r.ok; i++) {
            RemotePaneInfo p;
            p.id = r.u32();
            p.cols = r.u16();
            p.rows = r.u16();
            panes.push_back(p);
        }
        if (r.ok && on_attach_ok) on_attach_ok(sid, panes);
        break;
    }
    case MsgType::SessionClosed:
        if (on_session_closed) on_session_closed(r.u32());
        break;
    case MsgType::PaneExited:
        if (on_pane_exited) on_pane_exited(r.u32());
        break;
    case MsgType::Error:
        if (on_error) on_error(r.str());
        break;
    case MsgType::TitleChanged:
    case MsgType::CwdChanged:
    case MsgType::Bell:
        // The replica's own parser regenerates these locally; the
        // control events exist for pickers/detached observers.
        break;
    default:
        break;
    }
}

void RemoteClient::send_frame(uint16_t channel, uint16_t type, const void *data, size_t len) {
    if (m_fd < 0) return;
    uint8_t hdr[proto::FRAME_HEADER_SIZE];
    proto::encode_frame_header(hdr, {(uint32_t)len, channel, type});
    m_out.append((const char *)hdr, sizeof hdr);
    m_out.append((const char *)data, len);
    flush();
}

void RemoteClient::flush() {
    while (m_fd >= 0 && m_out_off < m_out.size()) {
        ssize_t n = send(m_fd, m_out.data() + m_out_off, m_out.size() - m_out_off, MSG_NOSIGNAL);
        if (n > 0) { m_out_off += n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        fail();
        return;
    }
    if (m_fd < 0) return;
    if (m_out_off == m_out.size()) {
        m_out.clear();
        m_out_off = 0;
    }
    bool want = m_out_off < m_out.size();
    if (want != m_write_armed) {
        m_write_armed = want;
        m_loop.modify_fd(m_fd, want ? (EV_READ | EV_WRITE) : EV_READ);
    }
}

void RemoteClient::hello() {
    proto::Writer w;
    w.u32(proto::PROTO_VERSION);
    send_control((uint16_t)MsgType::Hello, w);
}

void RemoteClient::list_sessions() {
    send_control((uint16_t)MsgType::ListSessions, {});
}

void RemoteClient::create_session(const std::string &name, const std::string &cwd,
                                  int cols, int rows) {
    proto::Writer w;
    w.str(name);
    w.str(cwd);
    w.u16((uint16_t)cols);
    w.u16((uint16_t)rows);
    send_control((uint16_t)MsgType::CreateSession, w);
}

void RemoteClient::attach(uint32_t sid) {
    proto::Writer w;
    w.u32(sid);
    send_control((uint16_t)MsgType::Attach, w);
}

void RemoteClient::detach() {
    send_control((uint16_t)MsgType::Detach, {});
}

void RemoteClient::resize(uint32_t pane_id, int cols, int rows) {
    proto::Writer w;
    w.u32(pane_id);
    w.u16((uint16_t)cols);
    w.u16((uint16_t)rows);
    send_control((uint16_t)MsgType::Resize, w);
}

void RemoteClient::kill_session(uint32_t sid) {
    proto::Writer w;
    w.u32(sid);
    send_control((uint16_t)MsgType::KillSession, w);
}

void RemoteClient::send_input(uint32_t pane_id, const char *data, size_t len) {
    send_frame((uint16_t)pane_id, proto::PANE_IN, data, len);
}

} // namespace rivt
