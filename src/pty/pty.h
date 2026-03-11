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

    // Write to PTY master
    int write(const char *buf, int len);
    int write(const std::string &s) { return write(s.data(), s.size()); }

    // Resize PTY
    void resize(int cols, int rows);

    // File descriptor for epoll
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
};

} // namespace rivt
