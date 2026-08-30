#include "pty/pty.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <pwd.h>

namespace rivt {

Pty::Pty() = default;

Pty::~Pty() {
    close();
}

bool Pty::spawn(int cols, int rows, const std::string &shell, const std::string &cwd,
                const std::string &auth_sock) {
    struct winsize ws {};
    ws.ws_col = cols;
    ws.ws_row = rows;

    pid_t pid = forkpty(&m_master_fd, nullptr, nullptr, &ws);
    if (pid < 0)
        return false;

    if (pid == 0) {
        // Child process
        std::string sh = shell;
        if (sh.empty()) {
            const char *env_shell = getenv("SHELL");
            if (env_shell) {
                sh = env_shell;
            } else {
                struct passwd *pw = getpwuid(getuid());
                if (pw && pw->pw_shell && pw->pw_shell[0])
                    sh = pw->pw_shell;
                else
                    sh = "/bin/sh";
            }
        }

        // Set environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "ghostty", 1);
        setenv("TERM_PROGRAM_VERSION", "1.2.0", 1);
        // rivtd sessions: agent requests bridge to the attached client.
        if (!auth_sock.empty()) setenv("SSH_AUTH_SOCK", auth_sock.c_str(), 1);

        // HACK: We claim to be Ghostty because apps like ink/claude-cli check
        // TERM_PROGRAM against a hardcoded whitelist to decide whether to enable
        // kitty keyboard protocol, instead of using the standard CSI ? u query.
        // We fully support the protocol, but there's no standard env var to
        // advertise that. This should be removed once the ecosystem moves to
        // runtime capability detection.

        // Change to working directory if specified
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                // Fall through to default CWD (inherited from parent)
            }
        }

        // Reset signal handlers
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGALRM, SIG_DFL);

        // Exec shell as login shell
        std::string login_shell = "-" + sh.substr(sh.rfind('/') + 1);
        execlp(sh.c_str(), login_shell.c_str(), nullptr);
        _exit(127);
    }

    // Parent
    m_child_pid = pid;

    // Set master fd to non-blocking
    int flags = fcntl(m_master_fd, F_GETFL);
    fcntl(m_master_fd, F_SETFL, flags | O_NONBLOCK);

    return true;
}

int Pty::read(char *buf, int max_len) {
    if (m_master_fd < 0) return -1;
    ssize_t n = ::read(m_master_fd, buf, max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return (int)n;
}

int Pty::write(const char *buf, int len) {
    if (m_master_fd < 0 || len < 0) return -1;

    size_t off = 0;
    // Fast path: nothing queued, so try to write directly. If anything is
    // already queued we must not write ahead of it — fall through to append.
    if (!has_pending()) {
        while (off < (size_t)len) {
            ssize_t n = ::write(m_master_fd, buf + off, (size_t)len - off);
            if (n > 0) { off += (size_t)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return -1;  // fatal (e.g. EIO after child exit)
        }
        if (off >= (size_t)len) return len;  // fully written, nothing queued
    }

    // Queue the remainder (or all of it) to flush once the fd is writable.
    m_write_buf.append(buf + off, (size_t)len - off);
    return len;
}

bool Pty::flush_writes() {
    if (m_master_fd < 0) { m_write_buf.clear(); m_write_off = 0; return false; }

    while (m_write_off < m_write_buf.size()) {
        ssize_t n = ::write(m_master_fd, m_write_buf.data() + m_write_off,
                            m_write_buf.size() - m_write_off);
        if (n > 0) { m_write_off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        // Fatal error: drop the queue rather than spin forever.
        m_write_buf.clear();
        m_write_off = 0;
        return false;
    }

    if (m_write_off >= m_write_buf.size()) {
        m_write_buf.clear();
        m_write_off = 0;
    }
    return has_pending();
}

void Pty::resize(int cols, int rows) {
    if (m_master_fd < 0) return;
    struct winsize ws {};
    ws.ws_col = cols;
    ws.ws_row = rows;
    ioctl(m_master_fd, TIOCSWINSZ, &ws);
}

bool Pty::alive() const {
    if (m_child_pid <= 0) return false;
    int status;
    pid_t result = waitpid(m_child_pid, &status, WNOHANG);
    return result == 0;
}

void Pty::close() {
    if (m_master_fd >= 0) {
        ::close(m_master_fd);
        m_master_fd = -1;
    }
    if (m_child_pid > 0) {
        kill(m_child_pid, SIGHUP);
        int status;
        waitpid(m_child_pid, &status, 0);
        m_child_pid = -1;
    }
}

} // namespace rivt
