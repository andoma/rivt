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

    bool is_pty_mode() const { return !!pty_write_; }

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
    std::function<void()> on_exit;

private:
    void on_readable();
    void parse_line(const std::string &line);
    static std::string decode_octal(const std::string &s);

    EventLoop &loop_;
    int write_fd_ = -1;
    int read_fd_ = -1;
    pid_t pid_ = -1;
    std::string line_buf_;

    // %begin/%end tracking
    bool in_block_ = false;
    int block_cmd_num_ = -1;
    std::string block_output_;

    // PTY mode: write callback instead of pipe
    std::function<void(const std::string &)> pty_write_;

    // Command response queue: one entry per command sent
    std::deque<ResponseCallback> response_queue_;
};

} // namespace rivt
