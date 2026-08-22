#pragma once
#include "core/event_loop.h"
#include "proto/wire.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rivt {

struct RemoteSessionInfo {
    uint32_t id;
    std::string name;
    uint32_t npanes;
};

struct RemotePaneGeom {
    uint32_t id;
    int x, y, cols, rows;  // cell units within the session grid
};

// Client side of the rivtd attach protocol: framing, command encoding,
// event dispatch. Sibling of TmuxClient. Owns no panes — that's
// RemoteController's job.
class RemoteClient {
public:
    explicit RemoteClient(EventLoop &loop) : m_loop(loop) {}
    ~RemoteClient() { close(); }

    static std::string default_socket_path();

    // Connect to rivtd; if autostart, spawn one (double-forked, detached)
    // when nobody is listening and retry briefly.
    bool connect(const std::string &path, bool autostart);
    void close();
    bool connected() const { return m_fd >= 0; }

    // Commands
    void hello();
    void list_sessions();
    void create_session(const std::string &name, const std::string &cwd, int cols, int rows);
    void attach(uint32_t session_id);
    void detach();
    void resize_session(int cols, int rows);
    void split(uint32_t pane_id, bool horizontal);  // false = side by side
    void close_pane(uint32_t pane_id);
    void new_window();
    void close_window(uint32_t window_id);
    void kill_session(uint32_t session_id);
    void send_input(uint32_t pane_id, const char *data, size_t len);

    // Events. on_disconnect may be invoked from inside the read
    // callback; receivers must defer destruction of this object
    // (same rule as TmuxClient).
    std::function<void()> on_hello_ok;
    std::function<void(const std::vector<RemoteSessionInfo> &)> on_session_list;
    std::function<void(uint32_t sid, uint32_t pane_id, const std::string &error)> on_session_created;
    std::function<void(uint32_t sid)> on_attach_ok;
    std::function<void(uint32_t sid, uint32_t wid)> on_window_added;
    std::function<void(uint32_t sid, uint32_t wid)> on_window_closed;
    std::function<void(uint32_t sid, uint32_t wid, int cols, int rows,
                       const std::vector<RemotePaneGeom> &)> on_layout;
    std::function<void(uint32_t pane_id, const uint8_t *, size_t)> on_snapshot;
    std::function<void(uint32_t pane_id, const char *, size_t)> on_output;
    std::function<void(uint32_t pane_id)> on_pane_exited;
    std::function<void(uint32_t sid)> on_session_closed;
    std::function<void(const std::string &)> on_error;
    std::function<void()> on_disconnect;

private:
    void on_event(uint32_t ev);
    void process();
    void dispatch_control(uint16_t type, const uint8_t *data, size_t len);
    void send_frame(uint16_t channel, uint16_t type, const void *data, size_t len);
    void send_control(uint16_t type, const proto::Writer &w) {
        send_frame(0, type, w.buf.data(), w.buf.size());
    }
    void flush();
    void fail();

    EventLoop &m_loop;
    int m_fd = -1;
    std::string m_in, m_out;
    size_t m_out_off = 0;
    bool m_write_armed = false;
    bool m_failing = false;
};

} // namespace rivt
