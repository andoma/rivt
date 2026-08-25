# rivt manual

rivt is a Linux/macOS terminal emulator that can also attach to
persistent terminal *sessions* running on other machines, reached by
name through a small coordination service, with no SSH and no VPN.

This manual covers day-to-day use and the one-time setup. For the
internal design see `doc/remote-design.md`.

---

## 1. Concepts

- **Device** — one machine. Has a persistent identity (a P-256 keypair
  + self-signed cert) created automatically on first run.
- **Device set** — your group of devices (like a Tailscale tailnet).
  One device *founds* the set; others *join* it with a one-time code.
  Membership is what grants trust between devices.
- **rendezvous** — a Cloudflare Worker (+ Durable Object) you host. It
  does two jobs: a **directory** (look up a device by name) and it
  stores the signed **membership log**. It is untrusted: it can help
  devices find each other but cannot read your sessions or join your
  set (every device verifies the membership log itself).
- **rivtd** — the session daemon. Runs on machines you want to reach
  *into* (dev boxes, your desktop). Owns the shells; sessions survive
  your connection dropping.
- **rivt** — the terminal UI. A plain local terminal, and the client
  that connects to remote `rivtd`s.

Trust (membership) and reachability (running a daemon) are independent:
a laptop can be a member that only connects out; a dev box is a member
that also listens.

---

## 2. Install

Default build type is Release. The build auto-detects whether it can
build the UI.

**Linux desktop/laptop (UI + daemon):**
```
cmake -B build -G Ninja .
cmake --build build
sudo cmake --install build      # installs rivt, rivtd, systemd unit
```
Needs: a C++20 compiler, CMake+Ninja, OpenSSL headers, and the UI deps
(freetype2, harfbuzz, fontconfig, xcb*, xkbcommon, EGL, GLESv2). Network
access is needed at configure time to fetch the pinned picoquic.

**Linux dev box / server (daemon only, no graphics stack):**
```
cmake -B build -G Ninja -DRIVT_UI=OFF .
cmake --build build
```
Builds `rivtd` alone; no freetype/xcb/GL required. Runtime needs only
libc/libstdc++/libcrypto.

**macOS (client only):**
```
brew install openssl@3
cmake -B build -G Ninja .
cmake --build build
```
Produces the `rivt` UI. There is no `rivtd` on macOS (it's a
connect-only client).

---

## 3. First-time setup

Do this once per device. You need your rendezvous Worker deployed (see
`spike/rendezvous/`, `wrangler deploy`) and its URL, e.g.
`https://rivt-rendezvous.<you>.workers.dev`.

### a. Found the set (once, on your main machine)

```
rivtd setup            # Linux desktop:  founds + runs the daemon
# or
rivt setup             # macOS/laptop:   founds, client only
```
Press **Enter** at the code prompt to found a new set. It asks for the
rendezvous URL once and saves it. On `rivtd` it also offers to install
the systemd service.

### b. Add every other device (paste one code)

On a device that is already a member:
```
rivt pair              # or: rivtd pair
        → prints:  rivt1_XXXX…       (valid 10 min, single use)
```
On the new device:
```
rivtd setup rivt1_XXXX…    # Linux box you want to reach into
# or
rivt setup rivt1_XXXX…     # macOS/laptop, connect-only
# or interactively: run `rivt setup` and paste at the prompt
```
The code carries the rendezvous URL, so the joining device needs **no
prior configuration**. Back on the pairing device, confirm the shown
name + fingerprint with `y`.

Typical layout:
- **desktop (Linux):** `rivtd setup` → Enter (founds the set, runs the daemon)
- **dev boxes (Linux):** `rivtd pair` on the desktop → `rivtd setup <code>` on the box
- **laptop (macOS):** `rivt pair` on the desktop → `rivt setup <code>` on the laptop

### c. Dev box as a background service

`rivtd setup` offers this; to do it by hand:
```
systemctl --user enable --now rivtd     # runs `rivtd --listen`
loginctl enable-linger                   # start at boot, no login needed
```

---

## 4. Daily use

### Local terminal
```
rivt
```
A plain terminal. No daemon, no persistence — close it and it's gone.

### Connect to a remote machine
```
rivt --connect devbox            # by name (directory lookup)
rivt --connect 10.0.0.5:7433     # or a direct address
```
Attaches to the newest session there (creating one if none). The
session lives in the remote `rivtd`: if your connection drops or you
close the window, it keeps running; reconnect to find it as you left
it, full scrollback and layout restored.

### Window / pane keys (local and remote)
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

### Opt-in local persistence
```
rivt --remote
```
Runs local sessions through a daemon so they survive UI restarts, and
resumes all of them on launch. Off by default — plain `rivt` is
ephemeral by design.

---

## 5. Managing devices

```
rivtd --fingerprint     # this device's identity + config paths
rivt pair / rivtd pair  # add another device
```
Removing a device (revocation) is signed into the membership log; a CLI
for it is pending. Each daemon re-syncs the set every ~60 s, so a newly
paired device becomes reachable shortly after; a box that was already
running may need `systemctl --user restart rivtd` to pick up a change
immediately (see Limitations).

---

## 6. Files and environment

Per device (override the base dirs with the env vars below):
- `~/.local/state/rivt/device_key.pem` — private device key (0600)
- `~/.local/state/rivt/device_cert.pem` — self-signed cert
- `~/.local/state/rivt/membership.log` — the verified device set
- `~/.config/rivt/rendezvous` — rendezvous URL (one line)
- `~/.config/rivt/authorized_certs.pem` — QUIC trust bundle, derived
  from the membership log (do not hand-edit)
- `$XDG_RUNTIME_DIR/rivt/daemon.sock` — local daemon socket

Environment:
- `RIVT_RENDEZVOUS` — rendezvous URL (overrides the config file)
- `RIVT_STATE_DIR`, `XDG_CONFIG_HOME` — relocate state/config (used for
  isolated test setups)
- `RIVT_QUIC_IDLE_MS` — QUIC idle timeout (default 60000)
- `RIVT_QUIC_DEBUG=1` — log QUIC packet/event trace to stderr

Regenerating identity: delete `device_key.pem` (new identity, must
re-pair). Delete only `device_cert.pem` to keep the key but refresh the
cert (needed once if you built before certs carried a unique subject).

---

## 7. Upgrading the daemon

```
rivtd --upgrade         # re-exec in place; sessions, PTYs, clients survive
```
Same PID, shells never notice, attached clients blink and re-attach.
Safe to run after installing a new build. Under systemd it also just
works (the exec preserves the unit's process).

---

## 8. Current limitations

- **Direct reachability only.** `rivt --connect` works when a device's
  advertised address is reachable from you (same LAN, or a public /
  port-forwarded `udp/7433`). Punching through NATs and jump hosts is
  in progress; until it lands, boxes behind NAT need a reachable
  address or a forward.
- **Membership reload.** Pair devices before starting the daemon, or
  restart the daemon after pairing, until in-place membership reload
  lands.
- **macOS is client-only** (no `rivtd`).
- **Kitty graphics** are not carried in remote session snapshots (they
  reappear when the app redraws).
