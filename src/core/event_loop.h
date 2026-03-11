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

    void request_quit() { m_quit = true; }
    bool should_quit() const { return m_quit; }

    // Timer support: returns timer id
    using TimerCallback = std::function<void()>;
    int add_timer(int interval_ms, TimerCallback cb, bool repeating = true);
    void remove_timer(int timer_id);

private:
    int m_epoll_fd;
    bool m_quit = false;

    struct FdEntry {
        int fd;
        Callback cb;
    };
    std::vector<FdEntry> m_fds;

    struct TimerEntry {
        int id;
        int fd;
        TimerCallback cb;
        bool repeating;
    };
    std::vector<TimerEntry> m_timers;
    int m_next_timer_id = 1;
};

} // namespace rivt
