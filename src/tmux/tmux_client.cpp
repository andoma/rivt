#include "tmux/tmux_client.h"
#include "core/debug.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <cstdio>
#include <cstring>

namespace rivt {

TmuxClient::TmuxClient(EventLoop &loop) : loop_(loop) {}

TmuxClient::~TmuxClient() {
    if (read_fd_ >= 0) {
        loop_.remove_fd(read_fd_);
        close(read_fd_);
    }
    if (write_fd_ >= 0) close(write_fd_);
    if (pid_ > 0) {
        // Don't kill tmux — we want the session to survive detach
        waitpid(pid_, nullptr, WNOHANG);
    }
}

bool TmuxClient::start(const std::vector<std::string> &args) {
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
        perror("pipe");
        return false;
    }

    pid_ = fork();
    if (pid_ < 0) {
        perror("fork");
        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return false;
    }

    if (pid_ == 0) {
        // Child: stdin from pipe_in[0], stdout/stderr to pipe_out[1]
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_out[1], STDERR_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);

        // Build argv: tmux -CC <args...>
        std::vector<const char *> argv;
        argv.push_back("tmux");
        argv.push_back("-CC");
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp("tmux", const_cast<char *const *>(argv.data()));
        perror("execvp tmux");
        _exit(127);
    }

    // Parent
    close(pipe_in[0]);
    close(pipe_out[1]);
    write_fd_ = pipe_in[1];
    read_fd_ = pipe_out[0];

    loop_.add_fd(read_fd_, [this](uint32_t events) {
        if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            on_readable();
        }
    });

    return true;
}

void TmuxClient::start_pty_mode(std::function<void(const std::string &)> write_fn) {
    pty_write_ = std::move(write_fn);
}

void TmuxClient::feed_data(const char *buf, size_t len) {
    line_buf_.append(buf, len);

    size_t pos;
    while ((pos = line_buf_.find('\n')) != std::string::npos) {
        std::string line = line_buf_.substr(0, pos);
        line_buf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        parse_line(line);
    }
}

void TmuxClient::on_readable() {
    char buf[65536];
    ssize_t n = read(read_fd_, buf, sizeof(buf));
    if (n <= 0) {
        if (on_exit) on_exit();
        return;
    }

    line_buf_.append(buf, n);

    // Extract complete lines
    size_t pos;
    while ((pos = line_buf_.find('\n')) != std::string::npos) {
        std::string line = line_buf_.substr(0, pos);
        line_buf_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        parse_line(line);
    }
}

void TmuxClient::parse_line(const std::string &line) {
    if (line.empty()) return;

    // Log non-output lines (output is too frequent)
    if (line[0] == '%' && line.substr(0, 7) != "%output") {
        dbg("tmux-client: << %s", line.c_str());
    }

    // Handle %begin/%end/%error blocks
    if (in_block_) {
        if (line.substr(0, 4) == "%end" || line.substr(0, 6) == "%error") {
            in_block_ = false;
            if (!response_queue_.empty()) {
                auto cb = std::move(response_queue_.front());
                response_queue_.pop_front();
                if (cb) cb(block_output_);
            }
            block_output_.clear();
            return;
        }
        block_output_ += line + "\n";
        return;
    }

    if (line.substr(0, 6) == "%begin") {
        in_block_ = true;
        block_output_.clear();
        return;
    }

    if (line.substr(0, 7) == "%output") {
        // %output %PANE_ID DATA
        // Find pane ID after "%output "
        size_t p = 8; // skip "%output "
        if (p >= line.size() || line[p] != '%') return;
        p++; // skip '%'
        size_t id_end = line.find(' ', p);
        if (id_end == std::string::npos) return;
        int pane_id = std::stoi(line.substr(p, id_end - p));
        std::string data = decode_octal(line.substr(id_end + 1));
        if (on_output) on_output(pane_id, data);
        return;
    }

    if (line.substr(0, 11) == "%window-add") {
        // %window-add @ID
        size_t p = 12;
        if (p >= line.size() || line[p] != '@') return;
        int window_id = std::stoi(line.substr(p + 1));
        if (on_window_add) on_window_add(window_id);
        return;
    }

    if (line.substr(0, 13) == "%window-close") {
        // %window-close @ID
        size_t p = 14;
        if (p >= line.size() || line[p] != '@') return;
        int window_id = std::stoi(line.substr(p + 1));
        if (on_window_close) on_window_close(window_id);
        return;
    }

    if (line.substr(0, 15) == "%window-renamed") {
        // %window-renamed @ID name
        size_t p = 16;
        if (p >= line.size() || line[p] != '@') return;
        size_t id_end = line.find(' ', p + 1);
        if (id_end == std::string::npos) return;
        int window_id = std::stoi(line.substr(p + 1, id_end - p - 1));
        std::string name = line.substr(id_end + 1);
        if (on_window_renamed) on_window_renamed(window_id, name);
        return;
    }

    if (line.substr(0, 16) == "%session-changed") {
        if (on_session_changed) on_session_changed();
        return;
    }

    if (line.substr(0, 14) == "%layout-change") {
        // %layout-change @ID layout_string [flags...]
        size_t p = 15;
        if (p >= line.size() || line[p] != '@') return;
        size_t id_end = line.find(' ', p + 1);
        if (id_end == std::string::npos) return;
        int window_id = std::stoi(line.substr(p + 1, id_end - p - 1));
        // Layout string is the next token
        size_t layout_end = line.find(' ', id_end + 1);
        std::string layout;
        if (layout_end == std::string::npos)
            layout = line.substr(id_end + 1);
        else
            layout = line.substr(id_end + 1, layout_end - id_end - 1);
        if (on_layout_change) on_layout_change(window_id, layout);
        return;
    }

    if (line.substr(0, 5) == "%exit") {
        if (on_exit) on_exit();
        return;
    }
}

std::string TmuxClient::decode_octal(const std::string &s) {
    std::string result;
    result.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '3' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            unsigned char c = (s[i+1] - '0') * 64 + (s[i+2] - '0') * 8 + (s[i+3] - '0');
            result += (char)c;
            i += 3;
        } else if (s[i] == '\\' && i + 1 < s.size() && s[i+1] == '\\') {
            result += '\\';
            i++;
        } else {
            result += s[i];
        }
    }
    return result;
}

void TmuxClient::send_command(const std::string &cmd, ResponseCallback cb) {
    dbg("tmux-client: >> %s", cmd.c_str());
    response_queue_.push_back(std::move(cb));
    std::string msg = cmd + "\n";
    if (pty_write_) {
        pty_write_(msg);
    } else if (write_fd_ >= 0) {
        if (::write(write_fd_, msg.data(), msg.size()) < 0) {
            // Connection to tmux lost
        }
    }
}

void TmuxClient::send_keys(int pane_id, const std::string &data) {
    if (data.empty()) return;
    if (!pty_write_ && write_fd_ < 0) return;

    std::string cmd = "send-keys -t %" + std::to_string(pane_id) + " -H";
    for (unsigned char c : data) {
        char hex[4];
        snprintf(hex, sizeof(hex), " %02x", c);
        cmd += hex;
    }
    send_command(cmd);
}

void TmuxClient::refresh_client_size(int cols, int rows) {
    send_command("refresh-client -C " + std::to_string(cols) + "x" + std::to_string(rows));
}

void TmuxClient::detach() {
    send_command("detach");
}

} // namespace rivt
