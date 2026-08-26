#pragma once
#include <fcntl.h>
#include <sys/socket.h>

namespace rivt::net {

// Portable socket() + close-on-exec (+ optional non-blocking).
// Linux can pass SOCK_CLOEXEC/SOCK_NONBLOCK in the type argument, but
// macOS/BSD cannot, so set the flags via fcntl afterward. Single-
// threaded use, so the tiny window before FD_CLOEXEC is set is benign.
inline int socket_cloexec(int domain, int type, int protocol, bool nonblock = false) {
    int fd = ::socket(domain, type, protocol);
    if (fd < 0) return -1;
    int fdflags = ::fcntl(fd, F_GETFD);
    if (fdflags != -1) ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    if (nonblock) {
        int flflags = ::fcntl(fd, F_GETFL);
        if (flflags != -1) ::fcntl(fd, F_SETFL, flflags | O_NONBLOCK);
    }
    return fd;
}

} // namespace rivt::net
