#!/bin/sh
# Mint short-lived Cloudflare TURN credentials.
# Needs ~/.config/rivt-spike.env with TURN_KEY_ID and TURN_KEY_API_TOKEN.
set -eu
. "${HOME}/.config/rivt-spike.env"
TTL="${1:-86400}"
curl -sf -X POST \
  "https://rtc.live.cloudflare.com/v1/turn/keys/${TURN_KEY_ID}/credentials/generate-ice-servers" \
  -H "Authorization: Bearer ${TURN_KEY_API_TOKEN}" \
  -H "Content-Type: application/json" \
  -d "{\"ttl\": ${TTL}}"
echo
