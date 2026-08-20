#pragma once
#include <cstdint>

namespace rivt::proto {

constexpr uint32_t PROTO_VERSION = 1;

// Frame types on pane channels (channel = pane id, > 0)
enum PaneFrame : uint16_t {
    PANE_OUT = 0,       // daemon -> client: raw VT output bytes
    PANE_IN = 1,        // client -> daemon: raw input bytes
    PANE_SNAPSHOT = 2,  // daemon -> client: proto::Snapshot blob
};

// Control messages (channel 0, type = MsgType). Payload encodings use
// proto::Writer primitives; str = u32 length + bytes.
enum class MsgType : uint16_t {
    // client -> daemon
    Hello = 1,          // u32 proto_version
    ListSessions = 2,   // -
    CreateSession = 3,  // str name, str cwd, u16 cols, u16 rows
    Attach = 4,         // u32 session_id
    Detach = 5,         // -
    Resize = 6,         // u32 pane_id, u16 cols, u16 rows
    KillSession = 7,    // u32 session_id

    // daemon -> client
    HelloOk = 64,        // u32 proto_version
    SessionList = 65,    // u32 n, n x { u32 id, str name, u32 npanes }
    SessionCreated = 66, // u32 session_id, u32 pane_id (0 = failed), str error
    AttachOk = 67,       // u32 session_id, u32 n, n x { u32 pane_id, u16 cols, u16 rows }
                         //   followed by one PANE_SNAPSHOT frame per pane
    SessionClosed = 68,  // u32 session_id
    PaneExited = 69,     // u32 pane_id
    TitleChanged = 70,   // u32 pane_id, str title
    CwdChanged = 71,     // u32 pane_id, str cwd (raw OSC 7 uri)
    Bell = 72,           // u32 pane_id
    Error = 73,          // str message
};

} // namespace rivt::proto
