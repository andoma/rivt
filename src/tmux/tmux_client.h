#pragma once
#include "core/event_loop.h"
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace rivt {

class TmuxClient {
public:
    TmuxClient(EventLoop &loop);
    ~TmuxClient();

    // Start by spawning tmux -CC as subprocess (for `rivt tmux` command)
    bool start(const std::vector<std::string> &args);

    // Start in PTY mode (tmux -CC running inside an existing PTY)
    void start_pty_mode(std::function<void(const std::string &)> write_fn);

    // Feed raw data from PTY (used in PTY mode)
    void feed_data(const char *buf, size_t len);

    bool is_pty_mode() const { return !!m_pty_write; }

    using ResponseCallback = std::function<void(const std::string &output)>;
    void send_command(const std::string &cmd, ResponseCallback cb = nullptr);
    void send_keys(int pane_id, const std::string &data);
    void refresh_client_size(int cols, int rows);
    void detach();

    // Callbacks (set by TmuxController)
    std::function<void(int pane_id, const std::string &data)> on_output;
    std::function<void(int window_id)> on_window_add;
    std::function<void(int window_id)> on_window_close;
    std::function<void(int window_id, const std::string &name)> on_window_renamed;
    std::function<void(int window_id, const std::string &layout, bool is_active)> on_layout_change;
    std::function<void()> on_session_changed;
    std::function<void(int window_id)> on_session_window_changed;
    std::function<void()> on_exit;

private:
    void on_readable();
    void parse_line(const std::string &line);
    static std::string decode_octal(const std::string &s);

    EventLoop &m_loop;
    int m_write_fd = -1;
    int m_read_fd = -1;
    pid_t m_pid = -1;
    std::string m_line_buf;

    // %begin/%end tracking
    bool m_in_block = false;
    int m_block_cmd_num = -1;
    std::string m_block_output;

    // PTY mode: write callback instead of pipe
    std::function<void(const std::string &)> m_pty_write;

    // Command response queue: one entry per command sent
    std::deque<ResponseCallback> m_response_queue;
};

} // namespace rivt
