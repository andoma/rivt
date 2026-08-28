#pragma once
//
// Central log sink. Everything user-visible or diagnostic goes through
// logmsg()/dbg(), never raw fprintf(stderr): stderr output gets a
// timestamp, and after log_to_syslog() (daemon mode) every message is
// also forwarded to syslog. Trailing newlines in the format are
// normalized away; one is always emitted on stderr.
//
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <syslog.h>

namespace rivt {

inline bool &debug_enabled() {
    static bool enabled = false;
    return enabled;
}

inline bool &log_syslog_enabled() {
    static bool enabled = false;
    return enabled;
}

// Daemon mode: also copy every message to syslog under `ident`.
inline void log_to_syslog(const char *ident) {
    openlog(ident, LOG_PID, LOG_DAEMON);
    log_syslog_enabled() = true;
}

inline void vlogmsg(const char *fmt, va_list ap) {
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);
    size_t len = strlen(msg);
    while (len && msg[len - 1] == '\n') msg[--len] = '\0';
    if (log_syslog_enabled()) syslog(LOG_INFO, "%s", msg);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03d %s\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), msg);
}

inline void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlogmsg(fmt, ap);
    va_end(ap);
}

inline void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void dbg(const char *fmt, ...) {
    if (!debug_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    vlogmsg(fmt, ap);
    va_end(ap);
}

} // namespace rivt
