# rivt remote: client/server design

Status: draft, 2026-08-18

Persistent terminal sessions hosted on any machine, attachable from any
other machine, N:M, with connections that survive network changes
(wifi -> cell, suspend/resume, NAT rebinds) without restarting anything.
No SSH, no VPN/overlay. NAT traversal via STUN/TURN, coordination via a
small hosted rendezvous service. All backend infrastructure runs on
Cloudflare; we operate zero servers.

## Goals

- Sessions (PTYs + scrollback) live in a daemon, survive UI exit/crash.
- Attach to any of your machines from any other; roster of what's online.
- Seamless roaming: a network switch causes at most a sub-second stall,
  never a reconnect the user has to perform.
- End-to-end encrypted; rendezvous and relay see ciphertext only.
- Reuse the existing pane model: client-side ScreenBuffer + VtParser
  render remote panes exactly like tmux -CC panes do today.

## Non-goals

- Multi-user sharing / collaboration. One device set, one owner.
- Web client, mobile client (protocol shouldn't preclude them).
- mosh-style predictive local echo (later; the client-side ScreenBuffer
  makes it tractable, and QUIC datagrams are available for it).

## Topology

Every machine runs one daemon, `rivtd`. There is no client/server
distinction at the infrastructure level: each daemon both hosts sessions
and originates attachments. The rivt UI always talks to its local rivtd
over a unix socket using the same attach protocol; local sessions get
persistence for free.

```
 rivt UI (macbook)                          rivt UI (desktop)
      | unix socket                              | unix socket
   rivtd (macbook) <=== QUIC, direct or ===> rivtd (desktop)
      |        \        via TURN relay      /        |
      |         \                          /         |
      |     stun/turn.cloudflare.com (UDP)           |
      |                                              |
      +---- wss ----> rendezvous Worker+DO <-- wss --+
                      (registry + signaling)
```

## Components

### rivtd

- Owns PTYs. Each session is a set of panes; each pane is the existing
  ScreenBuffer + VtParser pair, running headless (authoritative state,
  scrollback ring, no renderer).
- Listens on a unix socket for local UIs and on one UDP socket for QUIC.
- Holds a hibernating WebSocket to the rendezvous DO for presence and
  signaling.
- Maintains a standing TURN allocation as its always-reachable ingress
  (see Roaming).

### Rendezvous: Cloudflare Worker + Durable Object

One DO instance per device set. Daemons connect with the WebSocket
Hibernation API. Functions:

- **Registry**: device name, pubkey, online state, session summaries
  (for the attach picker). Persisted in DO SQLite storage.
- **Signaling**: ferries connect offers/answers, trickled path
  candidates, and TURN permission requests between daemons.
- **Credential minting**: exchanges a device-key challenge signature for
  short-lived Cloudflare TURN credentials (<= 48 h). The Cloudflare API
  token exists only inside the Worker.

The DO never sees session content. Everything it ferries is either
metadata (names, addresses) or opaque.

### Cloudflare STUN/TURN

- `stun.cloudflare.com`: reflexive address discovery.
- `turn.cloudflare.com:3478/udp` (alt 53/udp): relay fallback. Relays
  arbitrary UDP, so our QUIC flows through unchanged; the relay never
  terminates the connection. Per-client throttling starts ~50-100 Mbps /
  5-10 kpps: far above terminal traffic. 1000 GB/month egress free,
  then $0.05/GB.

## Identity and authentication

Four questions, answered separately: who is a device, who decides
membership, how peers authenticate each other, and how daemons
authenticate to the rendezvous.

### Device identity

Each device has an ECDSA P-256 keypair. Identity = the public key;
display fingerprints are a short hash. P-256 is the single supported
algorithm, chosen because it is what key-protection hardware actually
implements (TPM 2.0, Apple Secure Enclave); a second algorithm would
double the verify paths and test matrix for zero interop gain, since
we control every endpoint. Software keys sign with deterministic ECDSA
(RFC 6979) to remove the nonce-reuse footgun; hardware keys generate
nonces internally. Serialized keys and signatures carry one algorithm
ID byte, spec'd "must be 0x01 = ECDSA-P256", as format headroom only.

Key storage tiers (increasing protection, no protocol changes between
them):

1. **Baseline**: `~/.local/state/rivt/device_key`, 0600, held at
   runtime in `memfd_secret()` memory with `mlock`, `MADV_DONTDUMP`,
   `PR_SET_DUMPABLE=0`. Protects against swap, core dumps, and some
   kernel-memory disclosure classes.
2. **Sealed at rest**: TPM-bound encryption of the key file; via
   `systemd-creds` (`LoadCredentialEncrypted=`) when rivtd runs as a
   systemd user unit, zero TPM code in rivt. Protects against disk
   theft, backups, offline copying.
3. **Hardware-resident**: key generated inside the TPM (Linux) or
   Secure Enclave (macOS) and never present in system RAM; signing
   happens in hardware (tens of ms, irrelevant at our rate). A
   compromised daemon gets a signing oracle while running, never the
   key. Optionally fronted by a tiny `rivt-keyd` process so the
   VT-parsing daemon holds only a socket to the signer, not the key
   handle.

### Membership: a signed operation log, not a server-side list

The DO must not be the authority on who is in the device set: anyone
with the Cloudflare account (or Cloudflare itself) could then inject a
device. Instead, membership is a log of signed operations that every
daemon verifies independently:

- The founding device's key is the trust anchor (nothing special about
  it afterwards).
- `add {pubkey, name}` and `remove {pubkey}` ops are each signed by a
  device that is a member (and not removed) at that point in the log.
- The DO stores and distributes the log but cannot forge ops. Daemons
  replay and verify the chain locally and derive the member list from
  it; the roster the DO sends is a hint, never an authority.
- Removal: any remaining member can sign a `remove` (lost/stolen
  device). Daemons drop live connections from removed keys as soon as
  they see the op, and reject them at every handshake.

Names are display labels; the pubkey is the identity. Rejoining under
an old name is a new identity: it needs a fresh approval and leaves the
old key as a dead member until removed.

For fleet hygiene (many hosts, hosts that reinstall), `add` ops may
carry an optional `expires` timestamp. An expired key counts as
removed. A device added with expiry keeps itself alive by signing
`extend` ops for its own key (heartbeat, e.g. weekly): a live device
never expires, a dead or wiped one ages out with no human action, and
a stolen key gains nothing it wouldn't have had with a non-expiring
entry. Recommended for build machines; interactive devices are added
without expiry.

Accepted limitation: a malicious rendezvous can *withhold* new ops
(freezing a daemon's view, e.g. hiding a removal from one daemon) or
deny service. It cannot join, impersonate, or eavesdrop.

### Pairing (adding a device)

Magic-wormhole model, but with a high-entropy code so no PAKE is
needed (the code is copy-pasted or QR-scanned between your own
machines, never memorized):

1. On an existing member M: `rivt pair` creates a single-use invite
   `{set_id, invite_id, secret}` (128-bit secret, 10 min expiry),
   shown as one paste-able string. M stays connected, listening.
2. On the new device N: `rivt join <code>`. N connects to the DO's
   unauthenticated join endpoint addressed by `invite_id` and sends
   `{pubkey_N, name_N, hmac(secret, pubkey_N || name_N)}`.
3. The DO ferries it to M. M verifies the HMAC (proving N holds the
   secret; the DO cannot MITM without it), shows N's name and
   fingerprint for human confirmation, then signs `add {pubkey_N}`,
   and replies with the full membership log, HMAC-tagged with the same
   secret so N can trust its first copy of the log.
4. The op propagates via the DO to all daemons.

If a typeable short code is ever wanted, swap the HMAC binding for a
PAKE (CPace/SPAKE2); the flow is otherwise unchanged.

`pair` and `join` are daemon verbs, not UI verbs: a headless host
enrolls with `rivtd join <code>` over its bootstrap ssh session, and
the approval prompt appears on the inviting device. Any member can
mint invites.

Crypto dependency check: ECDSA P-256 (RFC 6979), HMAC-SHA-256, and
X.509 self-signed certs are all in libcrypto, already linked.

### Peer authentication (QUIC)

TLS 1.3 inside QUIC with self-signed P-256 certs whose key is the
device key (secp256r1 is the most mainstream certificate type; picotls
handles it natively, and TPM-resident keys plug in via the signing
callback). Certificate validation is replaced entirely by:
peer key must be a current member per the locally verified log. Both
sides check; there is no CA and no trust in transport metadata. TURN
and the DO only ever see ciphertext.

Authorization is symmetric by default: any member may attach to any
member. A per-device config allowlist can restrict inbound attach
(e.g. a work machine accepting only the laptop) without protocol
changes.

### Rendezvous authentication

Purpose is only abuse prevention and roster privacy; E2E security never
depends on it. On WebSocket connect the Worker sends a random nonce;
the daemon returns `sign(device_key, nonce || set_id || "rendezvous")`.
The DO verifies the signature (WebCrypto ECDSA P-256) against the member
list it derives from the stored op log, then tags the socket with the
pubkey. TURN credentials are minted only over an authenticated socket,
with short TTLs. The signed blob is bound to the nonce and a purpose
label, so it cannot be replayed or repurposed.

The spike's shared-secret token is exactly the placeholder this section
replaces.

### Auth flow by example

One-time: deploy the rendezvous Worker + TURN key to a Cloudflare
account. Then:

```
desktop$  rivt remote init --rendezvous https://rivt-rdv.example.workers.dev
            # generates device key, signs + publishes the genesis op
desktop$  rivt pair
            # prints single-use invite: set id + invite id + 128-bit secret
macbook$  rivt join rivt1:...        # generates key, sends pubkey HMAC-bound
desktop:  Pair new device 'macbook' (e1d8-40b7)? [y/N] y
            # signs add-op, returns HMAC-tagged membership log
buildbox$ rivtd join rivt1:...       # headless: same flow, approval on desktop
desktop$  rivt device remove macbook # stolen device: signs remove-op,
            # all daemons drop that key immediately and at every handshake
```

Human interaction happens exactly twice in a device's life: the `y`
at pairing and (at most) the removal. Attach, roaming, rendezvous
reconnects, TURN credential rotation: all machine-to-machine, no
passwords, no known_hosts/TOFU prompts. Losing every device loses the
set (no recovery back door); re-init and re-pair.

### Threat model summary

| Adversary | Gets | Does not get |
|---|---|---|
| Network / NAT / TURN relay | ciphertext, traffic timing | content, session access |
| Cloudflare account compromise | DoS, metadata (roster, timing), op withholding, TURN usage on your bill | joining the set, MITM of QUIC or pairing, content |
| Stolen device (still member) | full access as that device | survival past a `remove` op signed on any other device |
| Leaked TURN credential | relay usage on your bill until TTL | any rivt session access |

## Signaling protocol

JSON messages over the DO WebSocket. Small, low rate; the 20:1 message
billing ratio makes this effectively free.

| Message | Direction | Purpose |
|---|---|---|
| `hello {sig}` | daemon -> DO | authenticate, come online |
| `roster {devices[]}` | DO -> daemon | full roster on join + deltas |
| `sessions {summary}` | daemon -> DO | update advertised session list |
| `turn-creds {user, pass, ttl}` | DO -> daemon | minted TURN credentials |
| `connect {to, conn_id, candidates[]}` | daemon -> DO -> daemon | attach offer with initial candidates |
| `accept {conn_id, candidates[]}` | reverse | answer |
| `candidate {conn_id, addr}` | either | trickled candidate (new reflexive/relayed addr) |
| `refresh-path {conn_id, new_addr}` | either | roaming: ask peer to CreatePermission for my new address |

Candidates carry `{type: local|reflexive|relayed, ip, port}`. All
addresses are public or LAN; TURN permissions to private ranges are
blocked by Cloudflare, which is fine since relayed traffic only ever
targets reflexive addresses.

## Path establishment

Minimal ICE-lite, not RFC 8445:

1. Initiator gathers candidates: local interface addrs, reflexive addr
   (STUN Binding to stun.cloudflare.com from the QUIC socket), and its
   TURN relayed addr. Sends `connect`.
2. Responder does the same, replies `accept`, and both sides add TURN
   permissions for each other's reflexive addresses.
3. Both sides send STUN Binding probes to all peer candidates from the
   same UDP socket QUIC uses. STUN and QUIC demux by first byte
   (RFC 7983 style).
4. Path priority: LAN direct > WAN direct (punched) > relayed. The
   initiator starts the QUIC handshake on the best validated path;
   if a better path validates later, QUIC connection migration moves to
   it. If nothing punches within ~500 ms, go straight through TURN.

Expected outcomes: same-LAN attaches never touch the WAN; typical
home/office NAT pairs punch; symmetric NAT / CGNAT (cell!) falls back to
relay, which is exactly the roaming case anyway.

## Transport: QUIC mapping

Library: picoquic (C, strong connection-migration support, implements
the multipath extension for a later upgrade). One QUIC connection per
daemon pair, multiplexing any number of attached sessions.

Streams:

- **Stream 0, control** (bidi): CBOR frames, length-prefixed.
  `list-sessions`, `attach {session}`, `detach`, `create-session`,
  `kill-session`, `resize {pane, cols, rows}`, `scrollback-fetch
  {pane, from, count}`, `clipboard-set/get`, `snapshot-done`.
- **One bidi stream per attached pane**: server -> client carries a
  snapshot, then the raw PTY output byte stream; client -> server
  carries input bytes (keys already encoded by the client's input
  layer, same as the local PTY write path).

Attach semantics:

1. Client sends `attach`. Server serializes the pane's current
   ScreenBuffer (visible screen + cursor + modes + last N scrollback
   lines) as the snapshot frame on the pane stream, then switches the
   stream to live raw output.
2. Client loads the snapshot into its own ScreenBuffer and feeds
   subsequent bytes through its VtParser, identical to a local pane.
3. Older scrollback is fetched lazily via `scrollback-fetch` when the
   user scrolls past what the snapshot carried.

Multiple simultaneous attachments to one session are allowed; size
conflicts resolve tmux-style: the most recently active client's size
wins, others letterbox.

Flow control is QUIC's own per-stream flow control; a slow link
backpressures the PTY read for that pane exactly like the local case.
Keepalive: QUIC PING every 15 s (also keeps NAT bindings warm; TURN
allocations refresh per spec, credentials re-minted before the 48 h
expiry).

## Roaming

The one subtlety: both the punched direct path and the client's own TURN
allocation are bound to the client's old source address, so a network
switch kills both. The peer's TURN allocation survives (its network
didn't change). Recovery therefore targets the peer's relayed address:

States per QUIC connection: `DIRECT`, `RELAYED`, `RECOVERING`.

On local address change (netlink notification, or path timeout):

1. Reconnect the rendezvous WebSocket (TCP to nearest edge, fast).
2. STUN from the new network to learn the new reflexive address; send
   `refresh-path` so the peer adds a TURN permission for it.
3. Send QUIC packets to the peer's relayed transport address. QUIC path
   validation + migration completes; state = `RELAYED`. Total budget:
   one WebSocket reconnect + 2-3 RTTs, sub-second. Output produced
   during the gap was buffered by QUIC retransmission; nothing is lost.
4. In the background: re-allocate own TURN, re-run candidate exchange,
   re-punch. When a direct path validates, migrate back; state =
   `DIRECT`.

The same machinery handles the peer moving, NAT rebinds, and
suspend/resume (resume looks like an address change with extra staleness;
QUIC PTO probes detect it).

Later upgrade: multipath QUIC keeps the relayed path permanently
validated as a standby, cutting step 3 to zero and removing the stall
entirely. Architecture is unchanged; it's a transport flag.

## Security notes

- E2E: TLS 1.3 inside QUIC, peer pinned to device-set pubkeys. TURN and
  the DO relay/ferry ciphertext.
- rivtd's unix socket is 0700 in `$XDG_RUNTIME_DIR`.
- Blast radius of a leaked TURN credential: someone relays traffic on
  our Cloudflare account until the credential expires (<= 48 h). They
  still can't complete a QUIC handshake with any daemon.
- Device revocation: remove pubkey from the set in the DO; daemons drop
  connections from revoked keys on notification and at next handshake.

## SSH agent forwarding

Built in from day one: a shell in a session hosted on any daemon can use
the ssh-agent of whichever machine the human is sitting at, with no ssh
config involved.

- At session creation, rivtd creates a per-session unix socket
  (`$XDG_RUNTIME_DIR/rivt/agent-<session>.sock`, 0600) and sets
  `SSH_AUTH_SOCK` to it before spawning the shell. The path is stable
  for the session's lifetime; env vars are baked into running shells,
  so the socket must outlive attach/detach cycles while the routing
  behind it changes dynamically.
- Each connection accepted on that socket opens one dedicated bidi QUIC
  stream to the attached client's daemon, which connects to the
  client's own `$SSH_AUTH_SOCK` and proxies bytes. Pure byte relay: no
  agent-protocol parsing, no mux to build (QUIC is the mux), the
  stream closes when either side closes.
- Routing with multiple attached clients: the most-recently-active
  client that has forwarding enabled (same rule as OSC 52 clipboard
  routing: keys and clipboard follow the human). No eligible client
  attached: the connection is refused and ssh reports no agent,
  exactly like a disconnected `ForwardAgent` today.
- Local attach uses the identical path: the daemon proxies to the local
  agent, so sessions behave the same whether viewed locally or
  remotely.
- Security: forwarding means a session host can use your keys while you
  are attached. Within a personal device set that is usually intended,
  but it is opt-in per device in config, the socket is uid-checked via
  `SO_PEERCRED`, and the transport is already E2E with device-set
  pinning. Sensitive keys should be loaded with `ssh-add -c`
  (per-use confirmation) as usual.
- The stream open frame is generic (`forward {kind: ssh-agent}`), so
  gpg-agent or other unix-socket forwards reuse the mechanism later.

## Costs and limits

Everything fits free tiers at personal scale: TURN egress well under
1000 GB/month (terminal output; most traffic goes direct after
punching), DO WebSockets hibernate when idle, DO SQLite stores a few KB.
Worst case: Workers Paid, $5/month.

## Phasing

1. **rivtd + attach protocol over the unix socket.** Carve session
   ownership out of the UI process: headless ScreenBuffer/VtParser,
   snapshot + live-stream attach, lazy scrollback. Local persistence
   works; no networking yet. Includes the per-session agent socket +
   `SSH_AUTH_SOCK` plumbing (proxying to the local agent). This is the
   bulk of the refactor and is independently useful.
2. **QUIC between daemons, direct addressing only.** picoquic
   integration, device keys, cert pinning, stream mapping, agent
   forwarding over dedicated streams. Test on LAN / already-reachable
   hosts.
3. **Rendezvous Worker + DO.** Registry, signaling, pairing, TURN
   credential minting. Attach picker in the UI.
4. **Path establishment + TURN fallback.** STUN client (vendored,
   Binding + the TURN subset: Allocate/Refresh/CreatePermission/Send/
   Data/ChannelBind is a few hundred lines), punching, path priority.
5. **Roaming.** netlink watcher, refresh-path flow, migration state
   machine. This is the acceptance test: mid-`yes`-output wifi -> cell
   with no visible break beyond a sub-second stall.
6. **Later**: multipath QUIC standby path, predictive echo over QUIC
   datagrams, session sharing between devices as a UI affordance.

## Measured (spike results)

2026-08-18, desktop behind home NAT (1500 MTU uplink), `spike/`:

- STUN Binding to stun.cloudflare.com: 2.5 ms RTT to anycast edge.
- Full TURN flow (401 -> authenticated Allocate -> CreatePermission ->
  punch -> Send/Data relay loop) works with minted credentials.
  Realm `turn.cloudflare.com`, allocation lifetime **600 s** (refresh
  cadence for the standing allocation; permissions are 300 s per RFC).
- Relay RTT through Send/Data indications: **3.5-5 ms** (vs 2.5 ms
  direct STUN); relay overhead is negligible from this network.
- Datagram size through the relay: payloads up to **1436 bytes** pass;
  1444+ fail. 1436 + 36 bytes Send-indication overhead = 1472 = exactly
  this uplink's UDP max (1500 - 28), so the ceiling is the local path
  MTU, not a Cloudflare limit: the relay forwards everything the access
  network can carry. QUIC's 1200-byte handshake minimum fits with wide
  margin; cap QUIC max datagram at path-MTU minus 36 on relayed paths.

Rendezvous spike (`spike/rendezvous/`, deployed Worker + SQLite-backed
DO with WebSocket hibernation):

- Signaling ferry RTT (A -> DO -> B -> DO -> A): **9.5-18 ms**
  (median 12.3), i.e. ~5-6 ms per one-way signaling hop.
- TURN credential minting from inside the DO: works (note: a fresh
  secret version takes a moment to propagate; first request after
  `secret put` can hit the old version).
- Ping auto-response answered at the edge without waking the object;
  a connection held 5 min idle (pings only) stayed open, and the first
  real message after idling completed in **21 ms** including DO wake.

QUIC through the relay (`spike/turn_shim.c` + `run-quic-relay.sh`,
item 1b): an unmodified picoquicdemo client completed a full TLS/h3
handshake and a 1 MB transfer through the TURN path (client raw ->
relay -> Send/Data shim -> server). 1 MB in 42 ms (~190 Mbps burst,
above the documented 50-100 Mbps sustained throttle; fine for bursts).
1400-byte QUIC packets traversed the wrapped leg (1400 + 36 = 1436,
consistent with the measured ceiling). Design validated: the relay
needs no QUIC awareness and the shim logic is what rivtd's relayed-path
encapsulation will do.

Still to measure: migration under address change (item 2), TURN idle
timeout behavior (item 3), cross-network punch rate (item 4).

## Open questions

- Snapshot encoding: reuse a compact binary dump of Cell rows vs.
  re-synthesizing a VT byte stream that reproduces the screen (tmux
  does the latter for -CC attach). Binary dump is simpler and lossless
  (exact attributes, wrapped flags); lean that way.
- Does `create-session` spawn the user's login shell via the existing
  pty/ forkpty wrapper as-is, or do we need PAM/logind session
  bookkeeping for long-lived detached sessions? (tmux gets away
  without; probably fine.)
- Scrollback limits per detached pane: unbounded ring vs. configured
  cap; interaction with existing `Config` scrollback setting.
- Cloudflare TURN idle timeout behavior is undocumented; measure, and
  size the keepalive accordingly. (Allocation lifetime measured at
  600 s; idle behavior within that window still untested.)
