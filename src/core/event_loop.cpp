#include "core/event_loop.h"
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cerrno>

namespace rivt {

static uint32_t to_epoll(uint32_t flags) {
    uint32_t out = 0;
    if (flags & EV_READ)  out |= EPOLLIN;
    if (flags & EV_WRITE) out |= EPOLLOUT;
    return out;
}

static uint32_t from_epoll(uint32_t bits) {
    uint32_t out = 0;
    if (bits & EPOLLIN)  out |= EV_READ;
    if (bits & EPOLLOUT) out |= EV_WRITE;
    if (bits & EPOLLHUP) out |= EV_HUP;
    if (bits & EPOLLERR) out |= EV_ERR;
    return out;
}

EventLoop::EventLoop() {
    m_backend_fd = epoll_create1(EPOLL_CLOEXEC);
    if (m_backend_fd < 0)
        throw std::runtime_error("epoll_create1 failed");
}

EventLoop::~EventLoop() {
    for (auto &t : m_timers) {
        if (t.fd >= 0) close(t.fd);
    }
    close(m_backend_fd);
}

void EventLoop::add_fd(int fd, Callback cb, uint32_t events) {
    if (fd < 0) return;  // no-op (Cocoa returns -1 from event_fd())
    struct epoll_event ev {};
    ev.events = to_epoll(events);
    ev.data.fd = fd;
    if (epoll_ctl(m_backend_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::runtime_error("epoll_ctl ADD failed");
    m_fds.push_back({fd, std::move(cb)});
}

void EventLoop::modify_fd(int fd, uint32_t events) {
    if (fd < 0) return;
    struct epoll_event ev {};
    ev.events = to_epoll(events);
    ev.data.fd = fd;
    epoll_ctl(m_backend_fd, EPOLL_CTL_MOD, fd, &ev);
}

void EventLoop::remove_fd(int fd) {
    if (fd < 0) return;
    epoll_ctl(m_backend_fd, EPOLL_CTL_DEL, fd, nullptr);
    m_fds.erase(std::remove_if(m_fds.begin(), m_fds.end(),
        [fd](const FdEntry &e) { return e.fd == fd; }), m_fds.end());
}

bool EventLoop::poll(int timeout_ms) {
    struct epoll_event events[16];
    int n = epoll_wait(m_backend_fd, events, 16, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return !m_quit;
        throw std::runtime_error("epoll_wait failed");
    }

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;

        // Check timers first
        bool is_timer = false;
        for (auto &t : m_timers) {
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
        for (auto &entry : m_fds) {
            if (entry.fd == fd) {
                entry.cb(from_epoll(events[i].events));
                break;
            }
        }
    }

    return !m_quit;
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

    int id = m_next_timer_id++;
    m_timers.push_back({id, tfd, 0, std::move(cb), repeating});
    add_fd(tfd, [](uint32_t) {}, EV_READ);
    return id;
}

void EventLoop::remove_timer(int timer_id) {
    auto it = std::find_if(m_timers.begin(), m_timers.end(),
        [timer_id](const TimerEntry &e) { return e.id == timer_id; });
    if (it != m_timers.end()) {
        remove_fd(it->fd);
        close(it->fd);
        m_timers.erase(it);
    }
}

} // namespace rivt
