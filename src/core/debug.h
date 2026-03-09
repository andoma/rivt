#pragma once
#include <cstdio>
#include <cstdarg>

namespace rivt {

inline bool &debug_enabled() {
    static bool enabled = false;
    return enabled;
}

inline void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void dbg(const char *fmt, ...) {
    if (!debug_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

} // namespace rivt
