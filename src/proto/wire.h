#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rivt::proto {

// Little-endian binary writer/reader for the rivt protocol.
// Reader is fail-soft: out-of-bounds reads return 0/"" and latch ok=false,
// so callers validate once at the end instead of per field.

struct Writer {
    std::vector<uint8_t> buf;

    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void i32(int32_t v) { raw(&v, 4); }
    void bytes(const void *p, size_t n) { raw(p, n); }
    void str(const std::string &s) {
        u32((uint32_t)s.size());
        raw(s.data(), s.size());
    }

private:
    void raw(const void *p, size_t n) {
        // Little-endian hosts only (x86-64, aarch64) — matches the rest
        // of the codebase's assumptions.
        const uint8_t *b = (const uint8_t *)p;
        buf.insert(buf.end(), b, b + n);
    }
};

struct Reader {
    const uint8_t *p, *end;
    bool ok = true;

    Reader(const uint8_t *data, size_t len) : p(data), end(data + len) {}

    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }

    std::string str() {
        uint32_t n = u32();
        if (!ok || (size_t)(end - p) < n) { ok = false; return {}; }
        std::string s((const char *)p, n);
        p += n;
        return s;
    }

    size_t remaining() const { return (size_t)(end - p); }
    void skip(size_t n) {
        if ((size_t)(end - p) < n) { ok = false; p = end; return; }
        p += n;
    }

private:
    void raw(void *out, size_t n) {
        if (!ok || (size_t)(end - p) < n) { ok = false; return; }
        memcpy(out, p, n);
        p += n;
    }
};

} // namespace rivt::proto
