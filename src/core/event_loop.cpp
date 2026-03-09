#include "core/event_loop.h"
#include <unistd.h>
#include <sys/timerfd.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace rivt {

EventLoop::EventLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error("epoll_create1 failed");
}

EventLoop::~EventLoop() {
    for (auto &t : timers_)
        close(t.fd);
    close(epoll_fd_);
}

void EventLoop::add_fd(int fd, Callback cb, uint32_t events) {
    struct epoll_event ev {};
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::runtime_error("epoll_ctl ADD failed");
    fds_.push_back({fd, std::move(cb)});
}

void EventLoop::remove_fd(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    fds_.erase(std::remove_if(fds_.begin(), fds_.end(),
        [fd](const FdEntry &e) { return e.fd == fd; }), fds_.end());
}

bool EventLoop::poll(int timeout_ms) {
    struct epoll_event events[16];
    int n = epoll_wait(epoll_fd_, events, 16, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return !quit_;
        throw std::runtime_error("epoll_wait failed");
    }

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;

        // Check timers first
        bool is_timer = false;
        for (auto &t : timers_) {
            if (t.fd == fd) {
                uint64_t expirations;
                if (read(t.fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
                    t.cb();
                    if (!t.repeating) {
                        remove_timer(t.id);
                    }
                }
                is_timer = true;
                break;
            }
        }
        if (is_timer) continue;

        // Regular fd callbacks
        for (auto &entry : fds_) {
            if (entry.fd == fd) {
                entry.cb(events[i].events);
                break;
            }
        }
    }

    return !quit_;
}

int EventLoop::add_timer(int interval_ms, TimerCallback cb, bool repeating) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
        throw std::runtime_error("timerfd_create failed");

    struct itimerspec ts {};
    ts.it_value.tv_sec = interval_ms / 1000;
    ts.it_value.tv_nsec = (interval_ms % 1000) * 1000000L;
    if (repeating) {
        ts.it_interval = ts.it_value;
    }
    timerfd_settime(tfd, 0, &ts, nullptr);

    int id = next_timer_id_++;
    timers_.push_back({id, tfd, std::move(cb), repeating});
    add_fd(tfd, [](uint32_t) {}, EPOLLIN);
    return id;
}

void EventLoop::remove_timer(int timer_id) {
    auto it = std::find_if(timers_.begin(), timers_.end(),
        [timer_id](const TimerEntry &e) { return e.id == timer_id; });
    if (it != timers_.end()) {
        remove_fd(it->fd);
        close(it->fd);
        timers_.erase(it);
    }
}

} // namespace rivt
