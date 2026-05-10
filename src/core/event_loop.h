#pragma once
#include <functional>
#include <vector>
#include <cstdint>

namespace rivt {

// Platform-neutral event flags exposed to fd callbacks. The Linux
// (epoll) and macOS (kqueue) backends translate native event bits
// to this enum before invoking callbacks.
enum EventFlags : uint32_t {
    EV_READ  = 1u << 0,
    EV_WRITE = 1u << 1,
    EV_HUP   = 1u << 2,
    EV_ERR   = 1u << 3,
};

class EventLoop {
public:
    using Callback = std::function<void(uint32_t events)>;

    EventLoop();
    ~EventLoop();

    // Add fd. fd == -1 is a no-op (used by Cocoa where there's no
    // per-window event fd). Defaults to read-readiness only.
    void add_fd(int fd, Callback cb, uint32_t events = EV_READ);
    void remove_fd(int fd);

    // Run one iteration, blocking up to timeout_ms (-1 = forever).
    // Returns false if should quit.
    bool poll(int timeout_ms = -1);

    void request_quit() { m_quit = true; }
    bool should_quit() const { return m_quit; }

    using TimerCallback = std::function<void()>;
    int add_timer(int interval_ms, TimerCallback cb, bool repeating = true);
    void remove_timer(int timer_id);

private:
    int m_backend_fd;  // epoll fd on Linux, kqueue fd on macOS
    bool m_quit = false;

    struct FdEntry {
        int fd;
        Callback cb;
    };
    std::vector<FdEntry> m_fds;

    struct TimerEntry {
        int id;
        int fd;          // -1 on macOS (kqueue uses ident, no fd)
        int kq_ident;    // kqueue ident (macOS only); 0 on Linux
        TimerCallback cb;
        bool repeating;
    };
    std::vector<TimerEntry> m_timers;
    int m_next_timer_id = 1;
};

} // namespace rivt
