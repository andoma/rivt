#include "core/pane.h"
#include "platform/platform.h"
#include <sys/epoll.h>

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
    pty_.write(data);
}

void Pane::write(const char *data, size_t len) {
    pty_.write(data, (int)len);
}

void Pane::resize(int cols, int rows) {
    screen_.resize(cols, rows);
    pty_.resize(cols, rows);
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
        pty_.write(data);
    };

    screen_.on_osc52_read = [this, platform, &config](const std::string &sel) {
        if (!config.osc52_read) return;
        bool primary = (sel.find('p') != std::string::npos);
        std::string text = platform->get_clipboard(primary);
        std::string response = "\033]52;" + sel + ";" + base64_encode(text) + "\033\\";
        pty_.write(response);
    };
}

} // namespace rivt
