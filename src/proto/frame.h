#pragma once
#include <cstdint>
#include <cstring>

namespace rivt::proto {

// Channel-multiplexed framing for the attach connection.
// Channel 0 carries control messages (type = MsgType); channel N > 0
// carries pane N's byte stream (type = 0 output, 1 input).
// Over QUIC (phase 2) channels map to streams and this header goes away.

constexpr size_t FRAME_HEADER_SIZE = 8;
// Sanity cap per frame. Attach snapshots are the big ones: 2000 lines
// of heavy scrollback serialize to several MB (a 1MB cap once made the
// client treat a legitimate 1.7MB snapshot as garbage and disconnect).
// rivtd trims snapshot depth to stay under half of this.
constexpr uint32_t FRAME_MAX_LEN = 8 << 20;

struct FrameHeader {
    uint32_t len;      // payload length, excluding header
    uint16_t channel;
    uint16_t type;
};

inline void encode_frame_header(uint8_t out[FRAME_HEADER_SIZE], const FrameHeader &h) {
    memcpy(out, &h.len, 4);
    memcpy(out + 4, &h.channel, 2);
    memcpy(out + 6, &h.type, 2);
}

inline bool decode_frame_header(const uint8_t in[FRAME_HEADER_SIZE], FrameHeader &h) {
    memcpy(&h.len, in, 4);
    memcpy(&h.channel, in + 4, 2);
    memcpy(&h.type, in + 6, 2);
    return h.len <= FRAME_MAX_LEN;
}

} // namespace rivt::proto
