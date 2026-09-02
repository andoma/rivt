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
**founds** the set: at its join prompt, press Enter instead of pasting
a code, and give the rendezvous URL once. Every other device then
**joins** with a pairing code minted from an existing member
(`rivt pair` or `rivtd pair`).

---

## Server setup (`rivtd`, Linux)

### 1. Build & install
```
cmake -B build -G Ninja -DRIVT_UI=OFF .    # daemon only, no graphics deps
cmake --build build
```
No install step: `rivtd join`/`rivtd install` below copy the binary to
`~/.local/bin/rivtd`, so the service keeps working when the OS image is
refreshed but `$HOME` persists. (`sudo cmake --install build` still
works for a system-wide prefix.)
Needs a C++20 compiler, CMake+Ninja, OpenSSL headers, and network access
at configure time (fetches pinned picoquic). Runtime deps are just
libc/libstdc++/libcrypto.

### 2. Enroll and run
```
rivtd join <code>      # paste a code from `rivtd pair`/`rivt pair` on a member
```
This joins the set (learning the rendezvous URL from the code), then
offers to install and start the background service. Accept it, or run it
yourself:
```
rivtd install                            # ~/.local/bin/rivtd --listen (udp/7433)
sudo loginctl enable-linger $USER        # keep running at boot / after logout
```
Upgrading later is `cmake --build build && ./build/rivtd install`: the
new binary replaces `~/.local/bin/rivtd` and the running daemon re-execs
it with sessions intact.

That's it — the box publishes itself under its hostname and is now
reachable by name.

To make this the founding device instead, run `rivtd join` with no code
and press Enter.

### Surviving logout: linger, and hostile images

A `systemd --user` service dies with your last login unless lingering is
on for your user — that's the `loginctl enable-linger` above (use sudo:
non-root goes through polkit, which needs an agent and a login session
you may not have). Check it with:
```
loginctl show-user $USER -p Linger     # Linger=yes / Linger=no
```
The flag is a file under `/var/lib/systemd/linger/`, i.e. on the OS
image: a refreshed image resets it to `no` even though `~/.local/bin/rivtd`
and the unit in `$HOME` survive. Re-check after every refresh.

Minimal/provisioned images can fight back in layered ways: no
`pkttyagent` (non-root loginctl fails with a bare "No such file or
directory"), a polkitd that won't start, or users provided by an NSS
overlay that a long-running logind can't resolve (enable-linger returns
ENOENT even as root). For the last one, write the flag by hand and
restart logind so it re-reads NSS:
```
sudo touch /var/lib/systemd/linger/$USER
sudo systemctl restart systemd-logind
```
On provisioned dev boxes this is often the only variant that works, so
keep it around; it is what to run after each image refresh.

If the login stack is too broken to bother, bypass it entirely with a
system-level unit — pid1 resolves the user itself and no
logind/linger/polkit is involved:
```
sudo rivtd install --system     # per-user unit rivtd-<you>.service
```
Several users can do this on one box: units are per-user, control
sockets are per-uid, and a second daemon picks a free QUIC port
automatically (clients dial directory candidates, not a fixed port).

### Server admin
```
rivtd --fingerprint    # identity + config paths
rivtd pair             # mint a code to add another device
rivtd --upgrade        # re-exec a new build in place; sessions/PTYs/clients survive
rivtd --listen [port]  # run in the foreground (default udp/7433)
rivtd install          # copy to ~/.local/bin, (re)install the systemd --user
                       # service, upgrade a running daemon in place
rivtd install --system # system-level unit instead (see above)
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
rivt join <code>       # paste a code from `rivt pair`/`rivtd pair` on a member
```
Joins the set (rendezvous URL comes from the code) and writes this
device's trust bundle. No daemon, nothing to run in the background.

To make this the founding device instead, run `rivt join` with no code
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
