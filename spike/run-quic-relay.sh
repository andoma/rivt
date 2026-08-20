#!/bin/bash
# Spike item 1b: unmodified picoquic handshake + 1MB transfer through
# Cloudflare TURN. All on one host:
#   picoquicdemo client -> client-shim (raw) -> Cloudflare relay
#     -> server-shim (Send/Data unwrap) -> picoquicdemo server
set -eu
SPIKE="$(cd "$(dirname "$0")" && pwd)"
PICO="${PICO:-/tmp/claude-1000/-home-andoma-rivt/945f50b7-3254-43c5-844c-5d1f0db458a2/scratchpad/picoquic}"
TMP="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT

. "$HOME/.config/rivt-spike.env"
eval "$("$SPIKE/mint-creds.sh" 3600 | python3 -c "
import json,sys
d=json.load(sys.stdin)
for s in d['iceServers']:
    if 'username' in s:
        print(f\"export TURN_USER='{s['username']}' TURN_PASS='{s['credential']}'\")
        break
")"

"$PICO/build/picoquicdemo" -p 4443 -c "$PICO/certs/cert.pem" -k "$PICO/certs/key.pem" \
    > "$TMP/quic-server.log" 2>&1 &

mkfifo "$TMP/cfifo"
exec 9<>"$TMP/cfifo"
"$SPIKE/turn_shim" client 6000 <&9 > "$TMP/client-shim.out" 2>"$TMP/client-shim.err" &

for i in $(seq 50); do grep -q REFLEXIVE "$TMP/client-shim.out" 2>/dev/null && break; sleep 0.1; done
REFL=$(awk '/REFLEXIVE/{print $2}' "$TMP/client-shim.out")
echo "client reflexive: $REFL"

"$SPIKE/turn_shim" server "$REFL" 127.0.0.1:4443 > "$TMP/server-shim.out" 2>"$TMP/server-shim.err" &
for i in $(seq 100); do grep -q RELAYED "$TMP/server-shim.out" 2>/dev/null && break; sleep 0.1; done
RELAY=$(awk '/RELAYED/{print $2}' "$TMP/server-shim.out")
echo "relayed address:  $RELAY"

echo "$RELAY" >&9
sleep 0.3

echo "--- QUIC handshake + 1MB download through relay ---"
"$PICO/build/picoquicdemo" -D -m 1400 -n test 127.0.0.1 6000 "0:/1000000" 2>&1 | tail -20
echo "--- shim logs ---"
cat "$TMP/server-shim.err"
