# rivt manual

rivt lets you attach to persistent terminal *sessions* on other
machines, reached by name through a small coordination service, with no
SSH and no VPN. It is also a plain local terminal.

There are two roles, set up **independently**:

- **Server** — a machine you connect *into*. Runs the `rivtd` daemon,
  which owns the shells so sessions survive disconnects. Linux only.
- **Client** — a machine you connect *from*, using the `rivt` UI (which
  is also your local terminal). Linux or macOS.

A machine can be both: just do the server setup and the client setup on
it. There is no combined mode — the two are wholly separate.

Every machine, of either role, is a **device** in your **device set**
(like a Tailscale tailnet): membership is what lets devices trust each
other. See `doc/remote-design.md` for internals.

---

## Prerequisite: the rendezvous (once, ever)

Both roles need a **rendezvous**: a Cloudflare Worker + Durable Object
you host. It lets devices find each other by name and stores the signed
membership log. It is untrusted — it cannot read sessions or join your
set.

Deploy it once from `spike/rendezvous/` (`wrangler deploy`) and note its
URL, e.g. `https://rivt-rendezvous.<you>.workers.dev`. You never type
this URL on most devices: it travels inside pairing codes.

The very first device you set up (server or client, doesn't matter)
**founds** the set: at its setup prompt, press Enter instead of pasting
a code, and give the rendezvous URL once. Every other device then
**joins** with a pairing code minted from an existing member
(`rivt pair` or `rivtd pair`).

---

## Server setup (`rivtd`, Linux)

### 1. Build & install
```
cmake -B build -G Ninja -DRIVT_UI=OFF .    # daemon only, no graphics deps
cmake --build build
sudo cmake --install build                  # rivtd + systemd unit
```
Needs a C++20 compiler, CMake+Ninja, OpenSSL headers, and network access
at configure time (fetches pinned picoquic). Runtime deps are just
libc/libstdc++/libcrypto.

### 2. Enroll and run
```
rivtd setup <code>     # paste a code from `rivtd pair`/`rivt pair` on a member
```
This joins the set (learning the rendezvous URL from the code), then
offers to install and start the background service. Accept it, or run it
yourself:
```
systemctl --user enable --now rivtd     # runs `rivtd --listen` (udp/7433)
loginctl enable-linger                   # keep running at boot / after logout
```
That's it — the box publishes itself under its hostname and is now
reachable by name.

To make this the founding device instead, run `rivtd setup` with no code
and press Enter.

### Server admin
```
rivtd --fingerprint    # identity + config paths
rivtd pair             # mint a code to add another device
rivtd --upgrade        # re-exec a new build in place; sessions/PTYs/clients survive
rivtd --listen [port]  # run in the foreground (default udp/7433)
```

---

## Client setup (`rivt`, Linux or macOS)

### 1. Build
Linux:
```
cmake -B build -G Ninja .
cmake --build build && sudo cmake --install build
```
macOS:
```
brew install openssl@3
cmake -B build -G Ninja .
cmake --build build
```

### 2. Enroll
```
rivt setup <code>      # paste a code from `rivt pair`/`rivtd pair` on a member
```
Joins the set (rendezvous URL comes from the code) and writes this
device's trust bundle. No daemon, nothing to run in the background.

To make this the founding device instead, run `rivt setup` with no code
and press Enter.

### 3. Connect
```
rivt --connect devbox            # by name (via the directory)
rivt --connect 10.0.0.5:7433     # or a direct address
```
Attaches to the newest session on that server (creating one if none).
The session lives in the server's `rivtd`: drop the connection or close
the window and it keeps running; reconnect and it's exactly as you left
it, scrollback and split layout restored.

### Client admin
```
rivt pair              # mint a code to add another device
rivt join <code>       # (same as `rivt setup <code>` without the prompts)
rivt                   # plain local terminal (no daemon, no persistence)
```

---

## Using a rivt window

Applies to any rivt window, local or connected to a server:

| Keys | Action |
|---|---|
| Ctrl+Shift+N | new window |
| Ctrl+Shift+T | new tab |
| Ctrl+Shift+D / E | split right / below |
| Ctrl+Shift+W | close pane |
| Ctrl+Shift+←↑↓→ | move focus between panes |
| Alt+1…9 | switch tab |
| Ctrl+Shift+F | search scrollback |
| Ctrl+Shift+C / V | copy / paste |
| Ctrl+Shift + / − / 0 | font size up / down / reset |

---

## Reference

### Files (per device)
- `~/.local/state/rivt/device_key.pem` — private device key (0600)
- `~/.local/state/rivt/device_cert.pem` — self-signed cert
- `~/.local/state/rivt/membership.log` — the verified device set
- `~/.config/rivt/rendezvous` — rendezvous URL (one line)
- `~/.config/rivt/authorized_certs.pem` — QUIC trust bundle, derived
  from the membership log (never hand-edited)
- `$XDG_RUNTIME_DIR/rivt/daemon.sock` — local daemon socket (server)

### Environment
- `RIVT_RENDEZVOUS` — rendezvous URL (overrides the config file)
- `RIVT_STATE_DIR`, `XDG_CONFIG_HOME` — relocate state/config
- `RIVT_QUIC_IDLE_MS` — QUIC idle timeout (default 60000)
- `RIVT_QUIC_DEBUG=1` — QUIC packet/event trace to stderr

### Identity
Delete `device_key.pem` for a fresh identity (must re-pair). Delete only
`device_cert.pem` to keep the key and refresh the cert (do this once if
you built before certs carried a unique subject).

---

## Current limitations

- **Direct reachability only.** A client reaches a server when the
  server's advertised address is reachable (same LAN, or a public /
  port-forwarded `udp/7433`). NAT / jump-host traversal is in progress.
- **Membership reload.** Enroll a server before starting its daemon, or
  restart the daemon after pairing, until in-place membership reload
  lands.
- **Kitty graphics** are not carried in remote snapshots (they reappear
  when the app redraws).
