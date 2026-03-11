#include "core/pane.h"
#include "core/debug.h"
#include "platform/platform.h"
#include <sys/epoll.h>
#include <cstring>

namespace rivt {

Pane::Pane(int cols, int rows, const Config &config)
    : m_screen(cols, rows, config.scrollback_lines)
    , m_parser(m_screen)
{
}

Pane::~Pane() = default;

bool Pane::spawn_shell(EventLoop &loop, const std::string &start_cwd) {
    dbg("spawn_shell: cwd='%s'", start_cwd.c_str());
    if (!m_pty.spawn(m_screen.cols(), m_screen.rows(), "", start_cwd))
        return false;

    m_pty_fd_registered = m_pty.fd();
    loop.add_fd(m_pty.fd(), [this](uint32_t events) {
        if (events & EPOLLIN) {
            char buf[65536];
            int n;
            while ((n = m_pty.read(buf, sizeof(buf))) > 0) {
                // If in tmux PTY mode, forward all data to override
                if (m_pty_data_override) {
                    m_pty_data_override(buf, n);
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
                            m_parser.feed(buf, found - buf);
                            if (on_needs_render) on_needs_render();
                        }
                        // Fire callback — handler sets m_pty_data_override
                        on_tmux_control_mode(this);
                        // Feed remainder after marker to tmux client
                        const char *after = found + marker_len;
                        int remaining = n - (int)(after - buf);
                        if (remaining > 0 && m_pty_data_override) {
                            m_pty_data_override(after, remaining);
                        }
                        continue;
                    }
                }

                m_parser.feed(buf, n);
                if (on_needs_render) on_needs_render();
            }
            // Incrementally update search matches for new output
            if (m_screen.search.active && !m_screen.search.query.empty()) {
                m_screen.find_matches_incremental();
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
    if (m_pty_fd_registered >= 0) {
        loop.remove_fd(m_pty_fd_registered);
        m_pty_fd_registered = -1;
    }
}

void Pane::write(const std::string &data) {
    if (m_write_callback) m_write_callback(data);
    else m_pty.write(data);
}

void Pane::write(const char *data, size_t len) {
    if (m_write_callback) m_write_callback(std::string(data, len));
    else m_pty.write(data, (int)len);
}

void Pane::feed_data(const char *buf, size_t len) {
    m_parser.feed(buf, len);
    if (m_screen.search.active && !m_screen.search.query.empty())
        m_screen.find_matches_incremental();
    if (on_needs_render) on_needs_render();
}

void Pane::resize(int cols, int rows) {
    m_screen.resize(cols, rows);
    // Don't resize the PTY if this pane is a tmux gateway — the tmux
    // window controls the client size via refresh-client -C, and a
    // TIOCSWINSZ here would cause tmux to fight over the dimensions.
    if (m_pty_fd_registered >= 0 && !m_pty_data_override) m_pty.resize(cols, rows);
}

void Pane::setup_callbacks(Platform *platform, const Config &config) {
    m_screen.on_title_change = [this, platform](const std::string &title) {
        platform->set_title(title);
        if (on_title_change) on_title_change(this, title);
    };

    m_screen.on_cwd_change = [this](const std::string &uri) {
        dbg("OSC 7 received: '%s'", uri.c_str());
        // OSC 7 sends file://hostname/path — extract the path
        const std::string prefix = "file://";
        if (uri.substr(0, prefix.size()) == prefix) {
            auto slash = uri.find('/', prefix.size());
            if (slash != std::string::npos)
                cwd = uri.substr(slash);
            else
                cwd = uri.substr(prefix.size());
        } else {
            cwd = uri;
        }
        dbg("pane cwd set to: '%s'", cwd.c_str());
    };

    m_screen.on_osc52_write = [platform, &config](const std::string &sel, const std::string &base64, const std::string &mime_type) {
        if (!config.osc52_write) return;
        std::string data = base64_decode(base64);
        bool primary = (sel.find('p') != std::string::npos);
        if (!mime_type.empty() && mime_type != "text/plain") {
            platform->set_clipboard_data(data, mime_type, primary);
        } else {
            platform->set_clipboard(data, primary);
            if (!primary) platform->set_clipboard(data, false);
        }
    };

    m_screen.on_write_back = [this](const std::string &data) {
        write(data);
    };

    m_screen.on_osc52_read = [this, platform, &config](const std::string &sel, const std::string &mime_type) {
        if (!config.osc52_read) return;
        bool primary = (sel.find('p') != std::string::npos);
        std::string data;
        std::string response;
        if (!mime_type.empty() && mime_type != "text/plain") {
            data = platform->get_clipboard_data(mime_type, primary);
            response = "\033]52;" + sel + ";type=" + mime_type + ";" + base64_encode(data) + "\033\\";
        } else {
            data = platform->get_clipboard(primary);
            response = "\033]52;" + sel + ";" + base64_encode(data) + "\033\\";
        }
        write(response);
    };
}

} // namespace rivt
