#pragma once
#include <functional>
#include <vector>
#include <sys/epoll.h>

namespace rivt {

class EventLoop {
public:
    using Callback = std::function<void(uint32_t events)>;

    EventLoop();
    ~EventLoop();

    // Add fd to epoll with EPOLLIN by default
    void add_fd(int fd, Callback cb, uint32_t events = EPOLLIN);
    void remove_fd(int fd);

    // Run one iteration, blocking up to timeout_ms (-1 = forever)
    // Returns false if should quit
    bool poll(int timeout_ms = -1);

    void request_quit() { quit_ = true; }
    bool should_quit() const { return quit_; }

    // Timer support: returns timer id
    using TimerCallback = std::function<void()>;
    int add_timer(int interval_ms, TimerCallback cb, bool repeating = true);
    void remove_timer(int timer_id);

private:
    int epoll_fd_;
    bool quit_ = false;

    struct FdEntry {
        int fd;
        Callback cb;
    };
    std::vector<FdEntry> fds_;

    struct TimerEntry {
        int id;
        int fd;
        TimerCallback cb;
        bool repeating;
    };
    std::vector<TimerEntry> timers_;
    int next_timer_id_ = 1;
};

} // namespace rivt
