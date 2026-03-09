#include "core/util.h"

namespace rivt {

static int base64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

std::string base64_decode(const std::string &in) {
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        int v = base64_val(c);
        if (v < 0) continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out += (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

std::string base64_encode(const std::string &in) {
    static const char E[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) | c;
        bits += 8;
        while (bits >= 0) {
            out += E[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += E[(val << (-bits)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

} // namespace rivt
