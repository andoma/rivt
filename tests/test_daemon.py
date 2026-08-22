#!/usr/bin/env python3
"""Integration test for rivtd: session lifecycle, attach/snapshot,
input/output, PTY resize, persistence across reconnect.
Usage: test_daemon.py <path-to-rivtd>"""
import os, socket, struct, subprocess, sys, tempfile, time

RIVTD = sys.argv[1]
PROTO_VERSION = 2
# control message types
HELLO, LIST, CREATE, ATTACH, DETACH, RESIZE, KILL = 1, 2, 3, 4, 5, 6, 7
SPLIT, CLOSE_PANE, NEW_WINDOW, CLOSE_WINDOW = 8, 9, 10, 11
HELLO_OK, SESSION_LIST, SESSION_CREATED, ATTACH_OK = 64, 65, 66, 67
SESSION_CLOSED, PANE_EXITED, LAYOUT = 68, 69, 74
WINDOW_ADDED, WINDOW_CLOSED = 75, 76
PANE_OUT, PANE_IN, PANE_SNAPSHOT = 0, 1, 2

def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)

def enc_str(s):
    b = s.encode()
    return struct.pack("<I", len(b)) + b

class Conn:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.settimeout(10)
        self.buf = b""

    def frame(self, channel, ftype, payload=b""):
        self.s.sendall(struct.pack("<IHH", len(payload), channel, ftype) + payload)

    def recv_frame(self):
        while True:
            if len(self.buf) >= 8:
                ln, ch, ty = struct.unpack("<IHH", self.buf[:8])
                if len(self.buf) >= 8 + ln:
                    payload = self.buf[8:8 + ln]
                    self.buf = self.buf[8 + ln:]
                    return ch, ty, payload
            data = self.s.recv(65536)
            if not data:
                fail("connection closed by daemon")
            self.buf += data

    def expect_control(self, want_type):
        # Pane output can legitimately interleave with control messages;
        # skip pane-channel frames while waiting.
        while True:
            ch, ty, payload = self.recv_frame()
            if ch != 0:
                continue
            if ty != want_type:
                fail(f"expected control {want_type}, got control type {ty}")
            return payload

    def hello(self):
        self.frame(0, HELLO, struct.pack("<I", PROTO_VERSION))
        self.expect_control(HELLO_OK)

    def collect_output(self, pane, needle, timeout=10):
        """Read frames until needle appears in the pane's output bytes."""
        out = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            ch, ty, payload = self.recv_frame()
            if ch == pane and ty == PANE_OUT:
                out += payload
                if needle in out:
                    return out
        fail(f"timeout waiting for {needle!r} in pane output")

def parse_layout(payload):
    sid, wid, cols, rows, n = struct.unpack("<IIHHI", payload[:16])
    panes = []
    for i in range(n):
        panes.append(struct.unpack("<IHHHH", payload[16 + i*16 - 4*i: 32 + i*16 - 4*i])
                     if False else struct.unpack("<IHHHH", payload[16 + i*12: 28 + i*12]))
    return sid, wid, cols, rows, panes  # pane = (id, x, y, cols, rows)

def snapshot_text(blob):
    """Decode GRID_MAIN rows from a snapshot blob into a text string."""
    magic, ver = struct.unpack("<IB", blob[:5])
    assert magic == 0x504E5352 and ver == 1, "bad snapshot header"
    off = 5
    text = []
    while off < len(blob):
        tag = blob[off]; slen = struct.unpack("<I", blob[off+1:off+5])[0]
        body, off = blob[off+5:off+5+slen], off + 5 + slen
        if tag != 2:  # SEC_GRID_MAIN
            continue
        p = 0
        while p < len(body):
            _wrapped, _zone, ncells = struct.unpack("<BIH", body[p:p+7]); p += 7
            row, got = [], 0
            while got < ncells:
                run, cp = struct.unpack("<HI", body[p:p+6]); p += 18  # run+cell
                row += [chr(cp) if 32 <= cp < 0x110000 else " "] * run
                got += run
            text.append("".join(row))
    return "\n".join(text)

def main():
    tmp = tempfile.mkdtemp()
    sock = os.path.join(tmp, "daemon.sock")
    env = dict(os.environ, SHELL="/bin/sh")
    daemon = subprocess.Popen([RIVTD, "--socket", sock], env=env)
    try:
        for _ in range(100):
            if os.path.exists(sock): break
            time.sleep(0.05)
        else:
            fail("socket never appeared")

        # --- create + attach + echo roundtrip ---
        a = Conn(sock)
        a.hello()
        a.frame(0, CREATE, enc_str("t1") + enc_str("") + struct.pack("<HH", 80, 24))
        p = a.expect_control(SESSION_CREATED)
        sid, pane = struct.unpack("<II", p[:8])
        assert pane != 0, "session creation failed"

        a.frame(0, ATTACH, struct.pack("<I", sid))
        p = a.expect_control(ATTACH_OK)
        (rsid,) = struct.unpack("<I", p[:4])
        assert rsid == sid
        wsid, wid1 = struct.unpack("<II", a.expect_control(WINDOW_ADDED)[:8])
        assert wsid == sid
        _, lwid, lcols, lrows, lpanes = parse_layout(a.expect_control(LAYOUT))
        assert lwid == wid1 and (lcols, lrows) == (80, 24) and len(lpanes) == 1
        assert lpanes[0][0] == pane and lpanes[0][3] == 80 and lpanes[0][4] == 24
        ch, ty, snap = a.recv_frame()
        assert ch == pane and ty == PANE_SNAPSHOT and len(snap) > 100
        print(f"attach ok: session {sid} pane {pane}, snapshot {len(snap)} bytes")

        a.frame(pane, PANE_IN, b"printf 'OUT:%s\\n' rivt_marker_123\n")
        a.collect_output(pane, b"OUT:rivt_marker_123")
        print("echo roundtrip ok")

        # --- session resize relayouts and reaches the PTY ---
        a.frame(0, RESIZE, struct.pack("<HH", 100, 30))
        _, _, lcols, lrows, lpanes = parse_layout(a.expect_control(LAYOUT))
        assert (lcols, lrows) == (100, 30) and lpanes[0][3:] == (100, 30)
        a.frame(pane, PANE_IN, b"stty size\n")
        a.collect_output(pane, b"30 100")
        print("resize ok (stty reports 30 100)")

        # --- split: two panes side by side, both usable ---
        a.frame(0, SPLIT, struct.pack("<IB", pane, 0))
        _, _, _, _, lpanes = parse_layout(a.expect_control(LAYOUT))
        assert len(lpanes) == 2, f"expected 2 panes, got {lpanes}"
        pane2 = [g[0] for g in lpanes if g[0] != pane][0]
        widths = sorted(g[3] for g in lpanes)
        assert sum(widths) + 1 == 100, f"widths {widths} + divider != 100"
        a.frame(pane2, PANE_IN, b"printf 'P2:%s\\n' works\n")
        a.collect_output(pane2, b"P2:works")
        # first pane sees the narrower grid
        a.frame(pane, PANE_IN, b"stty size\n")
        a.collect_output(pane, str(lpanes[0][4]).encode() + b" " + str(lpanes[0][3]).encode())
        print(f"split ok: panes {pane},{pane2} widths {widths}")

        # --- close the split pane: layout collapses back ---
        a.frame(0, CLOSE_PANE, struct.pack("<I", pane2))
        got_exit, got_layout = False, False
        deadline = time.time() + 10
        while not (got_exit and got_layout) and time.time() < deadline:
            ch, ty, payload = a.recv_frame()
            if ch == 0 and ty == PANE_EXITED:
                got_exit = True
            if ch == 0 and ty == LAYOUT:
                _, _, _, _, lp = parse_layout(payload)
                if len(lp) == 1 and lp[0][3:] == (100, 30):
                    got_layout = True
        assert got_exit and got_layout
        print("close-pane ok: layout collapsed to full grid")

        # --- windows: new tab, use it, close it ---
        a.frame(0, NEW_WINDOW, b"")
        wsid, wid2 = struct.unpack("<II", a.expect_control(WINDOW_ADDED)[:8])
        _, lwid, _, _, lpanes = parse_layout(a.expect_control(LAYOUT))
        assert lwid == wid2 and wid2 != wid1 and len(lpanes) == 1
        pane3 = lpanes[0][0]
        a.frame(pane3, PANE_IN, b"printf 'W2:%s\\n' hello\n")
        a.collect_output(pane3, b"W2:hello")
        a.frame(0, CLOSE_WINDOW, struct.pack("<I", wid2))
        p = a.expect_control(WINDOW_CLOSED)
        _, cwid = struct.unpack("<II", p[:8])
        assert cwid == wid2
        print(f"windows ok: added {wid2}, pane {pane3} usable, closed")

        # --- persistence: reconnect, snapshot must contain earlier output ---
        a.s.close()
        time.sleep(0.2)
        b = Conn(sock)
        b.hello()
        b.frame(0, LIST, b"")
        p = b.expect_control(SESSION_LIST)
        (n,) = struct.unpack("<I", p[:4])
        assert n == 1, f"expected 1 session after reconnect, got {n}"

        b.frame(0, ATTACH, struct.pack("<I", sid))
        b.expect_control(ATTACH_OK)
        b.expect_control(WINDOW_ADDED)
        b.expect_control(LAYOUT)
        ch, ty, snap = b.recv_frame()
        assert ty == PANE_SNAPSHOT
        text = snapshot_text(snap)
        assert "OUT:rivt_marker_123" in text, f"marker missing from snapshot grid:\n{text}"
        print("persistence ok: snapshot after reconnect contains earlier output")

        # --- shell exit closes pane and session ---
        b.frame(pane, PANE_IN, b"exit\n")
        seen = set()
        deadline = time.time() + 10
        while (PANE_EXITED not in seen or SESSION_CLOSED not in seen) and time.time() < deadline:
            ch, ty, payload = b.recv_frame()
            if ch == 0:
                seen.add(ty)
        assert PANE_EXITED in seen and SESSION_CLOSED in seen
        print("exit ok: PaneExited + SessionClosed delivered")

        daemon.terminate()
        assert daemon.wait(timeout=5) in (0, -15)
        print("ALL DAEMON TESTS PASSED")
    finally:
        if daemon.poll() is None:
            daemon.kill()
            daemon.wait()

if __name__ == "__main__":
    main()
