// macOS EventLoop backend.
//
// Uses kqueue() for fd I/O and EVFILT_TIMER for timers. The kqueue fd is
// registered as a CFFileDescriptor source on the main run loop so that
// [NSApp nextEventMatchingMask:] wakes up on fd readiness as well as on
// NSEvents. poll() drives one NSApp pump per call, which keeps the
// portable while (!loop.should_quit()) loop in main.cpp unchanged.

#include "core/event_loop.h"

#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <vector>

namespace rivt {

namespace {

// Per-EventLoop CF runloop bridge. Lifetime tied to EventLoop instance.
struct CFBridge {
    CFFileDescriptorRef cffd = nullptr;
    CFRunLoopSourceRef src = nullptr;
};

static std::vector<std::pair<EventLoop *, CFBridge>> g_bridges;

static CFBridge &bridge_for(EventLoop *loop) {
    for (auto &p : g_bridges) if (p.first == loop) return p.second;
    g_bridges.push_back({loop, CFBridge{}});
    return g_bridges.back().second;
}

static void bridge_remove(EventLoop *loop) {
    g_bridges.erase(std::remove_if(g_bridges.begin(), g_bridges.end(),
        [loop](const auto &p) { return p.first == loop; }), g_bridges.end());
}

static void cf_kqueue_callback(CFFileDescriptorRef cffd,
                               CFOptionFlags /*types*/,
                               void * /*info*/) {
    // Just re-arm; the actual draining and dispatch happens in
    // EventLoop::poll. The runloop wakeup itself causes
    // [NSApp nextEventMatchingMask:] to return so the surrounding loop
    // can pick up the work.
    CFFileDescriptorEnableCallBacks(cffd, kCFFileDescriptorReadCallBack);
}

} // namespace

EventLoop::EventLoop() {
    m_backend_fd = kqueue();
    if (m_backend_fd < 0)
        throw std::runtime_error("kqueue() failed");
    int flags = fcntl(m_backend_fd, F_GETFD);
    if (flags >= 0) fcntl(m_backend_fd, F_SETFD, flags | FD_CLOEXEC);

    CFBridge &b = bridge_for(this);
    CFFileDescriptorContext ctx{0, this, nullptr, nullptr, nullptr};
    b.cffd = CFFileDescriptorCreate(kCFAllocatorDefault, m_backend_fd,
                                    /*closeOnInvalidate=*/false,
                                    cf_kqueue_callback, &ctx);
    if (b.cffd) {
        b.src = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, b.cffd, 0);
        if (b.src) {
            CFRunLoopAddSource(CFRunLoopGetMain(), b.src, kCFRunLoopDefaultMode);
            CFFileDescriptorEnableCallBacks(b.cffd, kCFFileDescriptorReadCallBack);
        }
    }
}

EventLoop::~EventLoop() {
    CFBridge &b = bridge_for(this);
    if (b.src) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), b.src, kCFRunLoopDefaultMode);
        CFRelease(b.src);
    }
    if (b.cffd) {
        CFFileDescriptorInvalidate(b.cffd);
        CFRelease(b.cffd);
    }
    bridge_remove(this);

    for (auto &t : m_timers) {
        struct kevent ke;
        EV_SET(&ke, t.kq_ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
        kevent(m_backend_fd, &ke, 1, nullptr, 0, nullptr);
    }
    close(m_backend_fd);
}

void EventLoop::add_fd(int fd, Callback cb, uint32_t events) {
    if (fd < 0) return;
    struct kevent changes[2];
    int n = 0;
    if (events & EV_READ)
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (events & EV_WRITE)
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (n == 0) return;
    if (kevent(m_backend_fd, changes, n, nullptr, 0, nullptr) < 0)
        throw std::runtime_error("kevent EV_ADD failed");
    m_fds.push_back({fd, std::move(cb)});
}

void EventLoop::remove_fd(int fd) {
    if (fd < 0) return;
    struct kevent ke;
    EV_SET(&ke, fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    kevent(m_backend_fd, &ke, 1, nullptr, 0, nullptr);
    EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(m_backend_fd, &ke, 1, nullptr, 0, nullptr);
    m_fds.erase(std::remove_if(m_fds.begin(), m_fds.end(),
        [fd](const FdEntry &e) { return e.fd == fd; }), m_fds.end());
}

namespace {

// Drain kqueue with the given timeout, dispatching callbacks. timeout==nullptr
// means non-blocking. Returns true if any events were dispatched.
template <typename FdLookup, typename TimerLookup, typename TimerRemove>
bool drain_kqueue(int kq, struct timespec *tsp,
                  FdLookup fd_lookup, TimerLookup timer_lookup,
                  TimerRemove timer_remove) {
    struct kevent events[16];
    int n = kevent(kq, nullptr, 0, events, 16, tsp);
    if (n <= 0) return false;
    for (int i = 0; i < n; i++) {
        if (events[i].filter == EVFILT_TIMER) {
            int id = (int)(uintptr_t)events[i].udata;
            EventLoop::TimerCallback cb;
            bool repeating = false;
            timer_lookup(id, cb, repeating);
            if (cb) cb();
            if (!repeating) timer_remove(id);
            continue;
        }
        int fd = (int)events[i].ident;
        uint32_t flags = 0;
        if (events[i].filter == EVFILT_READ)  flags |= EV_READ;
        if (events[i].filter == EVFILT_WRITE) flags |= EV_WRITE;
        if (events[i].flags & EV_EOF)         flags |= EV_HUP;
        if (events[i].flags & EV_ERROR)       flags |= EV_ERR;
        EventLoop::Callback cb;
        fd_lookup(fd, cb);
        if (cb) cb(flags);
    }
    return true;
}

} // namespace

bool EventLoop::poll(int timeout_ms) {
    auto fd_lookup = [this](int fd, Callback &out) {
        for (auto &entry : m_fds)
            if (entry.fd == fd) { out = entry.cb; return; }
    };
    auto timer_lookup = [this](int id, TimerCallback &out, bool &repeating) {
        for (auto &t : m_timers)
            if (t.id == id) { out = t.cb; repeating = t.repeating; return; }
    };
    auto timer_remove = [this](int id) { remove_timer(id); };

    @autoreleasepool {
        // Drain any already-ready kqueue events without blocking.
        for (int batch = 0; batch < 16; batch++) {
            struct timespec zero {0, 0};
            if (!drain_kqueue(m_backend_fd, &zero, fd_lookup, timer_lookup, timer_remove))
                break;
        }

        if (NSApp != nil) {
            NSDate *until = (timeout_ms < 0)
                ? [NSDate distantFuture]
                : [NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0];

            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:until
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            while (event) {
                [NSApp sendEvent:event];
                event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES];
            }

            // Drain any kqueue events that fired during the runloop spin.
            for (int batch = 0; batch < 16; batch++) {
                struct timespec zero {0, 0};
                if (!drain_kqueue(m_backend_fd, &zero, fd_lookup, timer_lookup, timer_remove))
                    break;
            }
        } else {
            // No NSApp yet — block on kqueue with the requested timeout.
            struct timespec ts;
            struct timespec *tsp = nullptr;
            if (timeout_ms >= 0) {
                ts.tv_sec = timeout_ms / 1000;
                ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
                tsp = &ts;
            }
            drain_kqueue(m_backend_fd, tsp, fd_lookup, timer_lookup, timer_remove);
        }
    }
    return !m_quit;
}

int EventLoop::add_timer(int interval_ms, TimerCallback cb, bool repeating) {
    int id = m_next_timer_id++;
    int ident = id;
    struct kevent ke;
    uint16_t flags = EV_ADD | EV_ENABLE;
    if (!repeating) flags |= EV_ONESHOT;
    EV_SET(&ke, ident, EVFILT_TIMER, flags, 0, interval_ms,
           (void *)(uintptr_t)id);
    if (kevent(m_backend_fd, &ke, 1, nullptr, 0, nullptr) < 0)
        throw std::runtime_error("kevent EVFILT_TIMER add failed");
    m_timers.push_back({id, -1, ident, std::move(cb), repeating});
    return id;
}

void EventLoop::remove_timer(int timer_id) {
    auto it = std::find_if(m_timers.begin(), m_timers.end(),
        [timer_id](const TimerEntry &e) { return e.id == timer_id; });
    if (it == m_timers.end()) return;
    struct kevent ke;
    EV_SET(&ke, it->kq_ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    kevent(m_backend_fd, &ke, 1, nullptr, 0, nullptr);
    m_timers.erase(it);
}

} // namespace rivt
