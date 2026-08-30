#pragma once
#include <cstdint>

namespace rivt::proto {

// Bump on ANY wire-format change (messages, payloads, snapshot framing).
// Daemon and client hard-reject mismatches with a clear error, so a
// stale running daemon fails loudly instead of rendering nothing.
constexpr uint32_t PROTO_VERSION = 5;  // 5: per-pane QUIC streams

// Frame types on pane channels (channel = pane id, > 0)
enum PaneFrame : uint16_t {
    PANE_OUT = 0,       // daemon -> client: raw VT output bytes
    PANE_IN = 1,        // client -> daemon: u32 seq, raw input bytes (v5+)
    PANE_SNAPSHOT = 2,  // daemon -> client: proto::Snapshot blob
    PANE_SCROLLBACK = 3,// daemon -> client: u32 start_abs, u32 n, n lines
    PANE_ACK = 4,       // daemon -> client: u32 seq — the output sent so
                        //   far reflects this client's input through seq
                        //   (predictive-echo confirmation anchor)
                        //   (Snapshot::encode_line each, oldest first)
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
    Resize = 6,         // u16 cols, u16 rows (session grid; server relayouts)
    KillSession = 7,    // u32 session_id
    Split = 8,          // u32 pane_id, u8 dir (0 = vertical/right, 1 = horizontal/below)
    ClosePane = 9,      // u32 pane_id
    NewWindow = 10,     // - (in the attached session)
    CloseWindow = 11,   // u32 window_id
    FetchScrollback = 12, // u32 pane_id, u32 end_abs (exclusive), u32 count
                          //   — absolute line indices; reply is a
                          //   PANE_SCROLLBACK frame, possibly clamped
    UpgradeDaemon = 13,   // - ; accepted even before Hello and across
                          //   protocol versions (it exists to upgrade
                          //   mismatched daemons): re-exec with sessions
                          //   preserved
    Ping = 14,            // - ; no reply. Exists to elicit a QUIC ACK
                          //   datagram: the client's link watchdog sends
                          //   it after wake / network change to verify
                          //   the path. Old daemons ignore unknown types.

    // daemon -> client
    HelloOk = 64,        // u32 proto_version
    SessionList = 65,    // u32 n, n x { u32 id, str name, u32 npanes }
    SessionCreated = 66, // u32 session_id, u32 pane_id (0 = failed), str error
    AttachOk = 67,       // u32 session_id — followed per window by
                         //   WindowAdded + LayoutUpdate, then one
                         //   PANE_SNAPSHOT frame per pane
    SessionClosed = 68,  // u32 session_id
    PaneExited = 69,     // u32 pane_id
    TitleChanged = 70,   // u32 pane_id, str title
    CwdChanged = 71,     // u32 pane_id, str cwd (raw OSC 7 uri)
    Bell = 72,           // u32 pane_id
    Error = 73,          // str message
    LayoutUpdate = 74,   // u32 session_id, u32 window_id, u16 cols, u16 rows,
                         //   u32 n, n x { u32 pane_id, u16 x, u16 y, u16 cols, u16 rows }
                         //   (cell units; 1-cell gaps are divider space)
    WindowAdded = 75,    // u32 session_id, u32 window_id
    WindowClosed = 76,   // u32 session_id, u32 window_id

    // SSH agent forwarding: connections to the session's SSH_AUTH_SOCK
    // on the daemon become streams bridged to the attached client's
    // local agent. Stream ids are daemon-allocated; Data/Close flow in
    // both directions. Old peers ignore unknown types, so mixed
    // versions just mean a dead agent socket (ssh falls back).
    AgentOpen = 80,      // daemon -> client: u32 stream_id
    AgentData = 81,      // both: u32 stream_id, raw bytes
    AgentClose = 82,     // both: u32 stream_id
};

} // namespace rivt::proto
