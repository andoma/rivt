#include "pty/pty.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
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

bool Pty::spawn(int cols, int rows, const std::string &shell) {
    struct winsize ws {};
    ws.ws_col = cols;
    ws.ws_row = rows;

    pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
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

        // HACK: We claim to be Ghostty because apps like ink/claude-cli check
        // TERM_PROGRAM against a hardcoded whitelist to decide whether to enable
        // kitty keyboard protocol, instead of using the standard CSI ? u query.
        // We fully support the protocol, but there's no standard env var to
        // advertise that. This should be removed once the ecosystem moves to
        // runtime capability detection.

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
    child_pid_ = pid;

    // Set master fd to non-blocking
    int flags = fcntl(master_fd_, F_GETFL);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);

    return true;
}

int Pty::read(char *buf, int max_len) {
    if (master_fd_ < 0) return -1;
    ssize_t n = ::read(master_fd_, buf, max_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return (int)n;
}

int Pty::write(const char *buf, int len) {
    if (master_fd_ < 0) return -1;
    ssize_t n = ::write(master_fd_, buf, len);
    return (int)n;
}

void Pty::resize(int cols, int rows) {
    if (master_fd_ < 0) return;
    struct winsize ws {};
    ws.ws_col = cols;
    ws.ws_row = rows;
    ioctl(master_fd_, TIOCSWINSZ, &ws);
}

bool Pty::alive() const {
    if (child_pid_ <= 0) return false;
    int status;
    pid_t result = waitpid(child_pid_, &status, WNOHANG);
    return result == 0;
}

void Pty::close() {
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
    if (child_pid_ > 0) {
        kill(child_pid_, SIGHUP);
        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
}

} // namespace rivt
