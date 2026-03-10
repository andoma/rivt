#include "core/pane.h"
#include "platform/platform.h"
#include <sys/epoll.h>
#include <cstring>

namespace rivt {

Pane::Pane(int cols, int rows, const Config &config)
    : screen_(cols, rows, config.scrollback_lines)
    , parser_(screen_)
{
}

Pane::~Pane() = default;

bool Pane::spawn_shell(EventLoop &loop) {
    if (!pty_.spawn(screen_.cols(), screen_.rows()))
        return false;

    pty_fd_registered_ = pty_.fd();
    loop.add_fd(pty_.fd(), [this](uint32_t events) {
        if (events & EPOLLIN) {
            char buf[65536];
            int n;
            while ((n = pty_.read(buf, sizeof(buf))) > 0) {
                // If in tmux PTY mode, forward all data to override
                if (pty_data_override_) {
                    pty_data_override_(buf, n);
                    continue;
                }

                // Check for tmux control mode DCS: \033P1000p
                if (on_tmux_control_mode) {
                    static const char marker[] = "\033P1000p";
                    static const int marker_len = 7;
                    const char *found = nullptr;
                    for (int i = 0; i <= n - marker_len; i++) {
                        if (buf[i] == '\033' && memcmp(buf + i, marker, marker_len) == 0) {
                            found = buf + i;
                            break;
                        }
                    }
                    if (found) {
                        // Feed data before marker to VtParser
                        if (found > buf) {
                            parser_.feed(buf, found - buf);
                            if (on_needs_render) on_needs_render();
                        }
                        // Fire callback — handler sets pty_data_override_
                        on_tmux_control_mode(this);
                        // Feed remainder after marker to tmux client
                        const char *after = found + marker_len;
                        int remaining = n - (int)(after - buf);
                        if (remaining > 0 && pty_data_override_) {
                            pty_data_override_(after, remaining);
                        }
                        continue;
                    }
                }

                parser_.feed(buf, n);
                if (on_needs_render) on_needs_render();
            }
            // Incrementally update search matches for new output
            if (screen_.search.active && !screen_.search.query.empty()) {
                screen_.find_matches_incremental();
            }
            if (n < 0) {
                if (on_dead) on_dead(this);
            }
        }
        if (events & (EPOLLHUP | EPOLLERR)) {
            if (on_dead) on_dead(this);
        }
    });

    return true;
}

void Pane::detach(EventLoop &loop) {
    if (pty_fd_registered_ >= 0) {
        loop.remove_fd(pty_fd_registered_);
        pty_fd_registered_ = -1;
    }
}

void Pane::write(const std::string &data) {
    if (write_callback_) write_callback_(data);
    else pty_.write(data);
}

void Pane::write(const char *data, size_t len) {
    if (write_callback_) write_callback_(std::string(data, len));
    else pty_.write(data, (int)len);
}

void Pane::feed_data(const char *buf, size_t len) {
    parser_.feed(buf, len);
    if (screen_.search.active && !screen_.search.query.empty())
        screen_.find_matches_incremental();
    if (on_needs_render) on_needs_render();
}

void Pane::resize(int cols, int rows) {
    screen_.resize(cols, rows);
    // Don't resize the PTY if this pane is a tmux gateway — the tmux
    // window controls the client size via refresh-client -C, and a
    // TIOCSWINSZ here would cause tmux to fight over the dimensions.
    if (pty_fd_registered_ >= 0 && !pty_data_override_) pty_.resize(cols, rows);
}

void Pane::setup_callbacks(Platform *platform, const Config &config) {
    screen_.on_title_change = [this, platform](const std::string &title) {
        platform->set_title(title);
        if (on_title_change) on_title_change(this, title);
    };

    screen_.on_osc52_write = [platform, &config](const std::string &sel, const std::string &base64) {
        if (!config.osc52_write) return;
        std::string text = base64_decode(base64);
        bool primary = (sel.find('p') != std::string::npos);
        platform->set_clipboard(text, primary);
        if (!primary) platform->set_clipboard(text, false);
    };

    screen_.on_write_back = [this](const std::string &data) {
        write(data);
    };

    screen_.on_osc52_read = [this, platform, &config](const std::string &sel) {
        if (!config.osc52_read) return;
        bool primary = (sel.find('p') != std::string::npos);
        std::string text = platform->get_clipboard(primary);
        std::string response = "\033]52;" + sel + ";" + base64_encode(text) + "\033\\";
        write(response);
    };
}

} // namespace rivt
