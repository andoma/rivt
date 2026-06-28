#pragma once
#include <string>
#include <sys/types.h>

namespace rivt {

class Pty {
public:
    Pty();
    ~Pty();

    // Spawn a shell process, returns true on success
    bool spawn(int cols, int rows, const std::string &shell = "", const std::string &cwd = "");

    // Read from PTY master, returns bytes read (0 = would block, -1 = error/closed)
    int read(char *buf, int max_len);

    // Write to PTY master. The master fd is non-blocking, so a large write
    // (e.g. a paste) can be accepted only partially by the kernel. Any tail
    // that doesn't fit is queued and must be drained later with
    // flush_writes() once the fd reports writable. Returns len on success
    // (queued bytes count as accepted), or -1 on a fatal error.
    int write(const char *buf, int len);
    int write(const std::string &s) { return write(s.data(), s.size()); }

    // Drain queued bytes into the PTY. Returns true if data still remains
    // queued (fd would block again), false once the queue is empty.
    bool flush_writes();

    // True while there is queued data waiting for the fd to become writable.
    bool has_pending() const { return m_write_off < m_write_buf.size(); }

    // Resize PTY
    void resize(int cols, int rows);

    // File descriptor for the EventLoop (epoll on Linux, kqueue on macOS).
    int fd() const { return m_master_fd; }

    // Check if child is still alive
    bool alive() const;

    // Get child PID
    pid_t child_pid() const { return m_child_pid; }

    // Close PTY
    void close();

private:
    int m_master_fd = -1;
    pid_t m_child_pid = -1;

    // Queue of bytes not yet accepted by the kernel. m_write_off is the
    // offset of the first unwritten byte (avoids erasing from the front).
    std::string m_write_buf;
    size_t m_write_off = 0;
};

} // namespace rivt
