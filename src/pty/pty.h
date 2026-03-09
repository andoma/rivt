#pragma once
#include <string>
#include <sys/types.h>

namespace rivt {

class Pty {
public:
    Pty();
    ~Pty();

    // Spawn a shell process, returns true on success
    bool spawn(int cols, int rows, const std::string &shell = "");

    // Read from PTY master, returns bytes read (0 = would block, -1 = error/closed)
    int read(char *buf, int max_len);

    // Write to PTY master
    int write(const char *buf, int len);
    int write(const std::string &s) { return write(s.data(), s.size()); }

    // Resize PTY
    void resize(int cols, int rows);

    // File descriptor for epoll
    int fd() const { return master_fd_; }

    // Check if child is still alive
    bool alive() const;

    // Get child PID
    pid_t child_pid() const { return child_pid_; }

    // Close PTY
    void close();

private:
    int master_fd_ = -1;
    pid_t child_pid_ = -1;
};

} // namespace rivt
