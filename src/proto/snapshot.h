#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace rivt {

class ScreenBuffer;
class VtParser;

namespace proto {

// Serializes the full replicable state of a pane's terminal: grid, alt
// screen, scrollback tail, cursor, modes, and the VtParser's transient
// state — so a snapshot is valid at any byte boundary of the output
// stream. Client-view state (viewport, selection, search) and kitty
// images are deliberately excluded.
class Snapshot {
public:
    // max_scrollback: number of most-recent scrollback lines to include
    // (-1 = all). Omitted older lines are counted so the client knows
    // how much more history the server holds.
    static std::vector<uint8_t> serialize(const ScreenBuffer &sb, const VtParser &vp,
                                          int max_scrollback = -1);

    // Overwrites sb/vp with the snapshot state (resizing as needed).
    // Returns false on malformed or version-incompatible input,
    // in which case sb/vp may be partially modified.
    static bool deserialize(ScreenBuffer &sb, VtParser &vp,
                            const uint8_t *data, size_t len);
};

} // namespace proto
} // namespace rivt
