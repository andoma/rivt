#include "pty/pty.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <util.h>
#include <spawn.h>
#include <crt_externs.h>
#include <string>
#include <vector>
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
    // Exec shell as login shell.
    // HACK: We claim to be Ghostty because apps like ink/claude-cli check
    // TERM_PROGRAM against a hardcoded whitelist to decide whether to enable
    // kitty keyboard protocol, instead of using the standard CSI ? u query.
    // We fully support the protocol, but there's no standard env var to
    // advertise that. This should be removed once the ecosystem moves to
    // runtime capability detection.
    std::string login_shell = "-" + sh.substr(sh.rfind('/') + 1);

#ifdef __APPLE__
    // No forkpty here: this process runs Network.framework worker threads
    // (nw_path_monitor), and fork's pthread_atfork child handlers abort
    // when another thread holds Network's necp lock at fork time
    // ("multi-threaded process forked"). posix_spawn never runs atfork
    // handlers, so the pty pair is set up via file actions instead.
    int slave_fd = -1;
    char slave_name[128];
    if (openpty(&m_master_fd, &slave_fd, slave_name, nullptr, &ws) < 0)
        return false;

    std::vector<std::string> env;
    auto skip = [](const char *s, const char *pfx) {
        return strncmp(s, pfx, strlen(pfx)) == 0;
    };
    for (char **e = *_NSGetEnviron(); *e; ++e) {
        const char *s = *e;
        if (skip(s, "TERM=") || skip(s, "COLORTERM=") ||
            skip(s, "TERM_PROGRAM=") || skip(s, "TERM_PROGRAM_VERSION="))
            continue;
        if (!auth_sock.empty() && skip(s, "SSH_AUTH_SOCK="))
            continue;
        env.push_back(s);
    }
    env.push_back("TERM=xterm-256color");
    env.push_back("COLORTERM=truecolor");
    env.push_back("TERM_PROGRAM=ghostty");
    env.push_back("TERM_PROGRAM_VERSION=1.2.0");
    // rivtd sessions: agent requests bridge to the attached client.
    if (!auth_sock.empty()) env.push_back("SSH_AUTH_SOCK=" + auth_sock);
    std::vector<char *> envp;
    for (auto &s : env) envp.push_back(s.data());
    envp.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Opening the slave by path (without O_NOCTTY) from the fresh session
    // leader makes it the controlling terminal.
    posix_spawn_file_actions_addopen(&fa, 0, slave_name, O_RDWR, 0);
    posix_spawn_file_actions_adddup2(&fa, 0, 1);
    posix_spawn_file_actions_adddup2(&fa, 0, 2);
    if (!cwd.empty())
        posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t sigdfl, sigempty;
    sigfillset(&sigdfl);
    sigemptyset(&sigempty);
    posix_spawnattr_setsigdefault(&attr, &sigdfl);
    posix_spawnattr_setsigmask(&attr, &sigempty);
    // CLOEXEC_DEFAULT: the shell inherits only the fds named in file
    // actions, not our sockets, kqueue, or the pty master.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID |
                                    POSIX_SPAWN_SETSIGDEF |
                                    POSIX_SPAWN_SETSIGMASK |
                                    POSIX_SPAWN_CLOEXEC_DEFAULT);

    char *argv[] = { const_cast<char *>(login_shell.c_str()), nullptr };
    pid_t pid;
    int rc = posix_spawnp(&pid, sh.c_str(), &fa, &attr, argv, envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(slave_fd);
    if (rc != 0) {
        ::close(m_master_fd);
        m_master_fd = -1;
        return false;
    }
    m_child_pid = pid;
#else
    pid_t pid = forkpty(&m_master_fd, nullptr, nullptr, &ws);
    if (pid < 0)
        return false;

    if (pid == 0) {
        // Child process
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "ghostty", 1);
        setenv("TERM_PROGRAM_VERSION", "1.2.0", 1);
        // rivtd sessions: agent requests bridge to the attached client.
        if (!auth_sock.empty()) setenv("SSH_AUTH_SOCK", auth_sock.c_str(), 1);

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

        execlp(sh.c_str(), login_shell.c_str(), nullptr);
        _exit(127);
    }

    // Parent
    m_child_pid = pid;
#endif

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
