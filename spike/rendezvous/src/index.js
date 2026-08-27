// rivt rendezvous spike: registry + signaling ferry + TURN credential minting.
//
// Spike auth is a shared secret (?token=...). The real design replaces this
// with an Ed25519 challenge signed by the device key.
//
// Secrets (wrangler secret put): SPIKE_TOKEN, TURN_KEY_ID, TURN_KEY_API_TOKEN

// --- Device directory (phase 3) ---------------------------------------
// Names bind to a device key (SPKI) on first registration; updates must
// be signed by that key. The directory is not part of the E2E trust:
// clients pin peer certificates, so a hostile directory can only DoS.

async function sha256hex(bytes) {
  const h = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function b64ToBytes(b64) {
  const bin = atob(b64.replace(/-/g, "+").replace(/_/g, "/"));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

async function verifyDeviceSig(spki_b64, sig_raw_b64, payload) {
  try {
    const key = await crypto.subtle.importKey(
      "spki", b64ToBytes(spki_b64),
      { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
    return await crypto.subtle.verify(
      { name: "ECDSA", hash: "SHA-256" }, key,
      b64ToBytes(sig_raw_b64), new TextEncoder().encode(payload));
  } catch {
    return false;
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/dir/") || url.pathname.startsWith("/log/") ||
        url.pathname.startsWith("/pair/")) {
      const id = env.RENDEZVOUS.idFromName("directory");
      return env.RENDEZVOUS.get(id).fetch(request);
    }
    if (url.pathname === "/ws") {
      // Single signaling instance so any two peers meet; addressed by
      // device signaling-id (?device=). Not authenticated — QUIC still
      // pins certs, so the ferry is just a rendezvous point.
      const id = env.RENDEZVOUS.idFromName("directory");
      return env.RENDEZVOUS.get(id).fetch(request);
    }
    return new Response("rivt rendezvous spike", { status: 200 });
  },
};

export class Rendezvous {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    // Answered at the edge without waking a hibernated object.
    this.ctx.setWebSocketAutoResponse(
      new WebSocketRequestResponsePair('{"type":"ping"}', '{"type":"pong"}'),
    );
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/dir/") || url.pathname.startsWith("/log/") ||
        url.pathname.startsWith("/pair/"))
      return this.directory(request, url);
    if (request.headers.get("Upgrade") !== "websocket")
      return new Response("expected websocket", { status: 426 });
    const device = new URL(request.url).searchParams.get("device") ?? "anon";
    const pair = new WebSocketPair();
    // Tag survives hibernation; it is our routing key.
    this.ctx.acceptWebSocket(pair[1], [device]);
    await this.ctx.storage.put(`seen:${device}`, Date.now());
    this.broadcast({ type: "joined", device }, device);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  // ---- device directory ----
  async directory(request, url) {
    const json = (obj, status = 200) =>
      new Response(JSON.stringify(obj), {
        status, headers: { "content-type": "application/json" } });

    if (url.pathname === "/dir/register" && request.method === "POST") {
      let b;
      try { b = await request.json(); } catch { return json({ error: "bad json" }, 400); }
      const { name, fingerprint, port, spki, sig, ts, addrs } = b;
      if (!name || !/^[a-zA-Z0-9_-]{1,64}$/.test(name) || !fingerprint || !port ||
          !spki || !sig || !ts)
        return json({ error: "missing fields" }, 400);
      if (Math.abs(Date.now() / 1000 - ts) > 300)
        return json({ error: "stale timestamp" }, 400);
      const payload = `${name}|${fingerprint}|${port}|${ts}`;
      if (!(await verifyDeviceSig(spki, sig, payload)))
        return json({ error: "bad signature" }, 403);

      const existing = await this.ctx.storage.get(`dev:${name}`);
      if (existing && existing.spki !== spki)
        return json({ error: "name is bound to another device key" }, 409);

      const observed = request.headers.get("CF-Connecting-IP") ?? "";
      const sig_id = await sha256hex(b64ToBytes(spki));  // signaling address
      await this.ctx.storage.put(`dev:${name}`, {
        name, fingerprint, port, spki, sig_id,
        addrs: Array.isArray(addrs) ? addrs.slice(0, 8) : [],
        observed_ip: observed,
        last_seen: Date.now(),
      });
      return json({ ok: true, observed_ip: observed, sig_id });
    }

    if (url.pathname === "/dir/lookup" && request.method === "GET") {
      const name = url.searchParams.get("name") ?? "";
      const d = await this.ctx.storage.get(`dev:${name}`);
      if (!d) return json({ error: "unknown device" }, 404);
      const { spki, ...pub } = d;  // sig_id stays in pub
      return json(pub);
    }

    // --- membership log: opaque, append-only, optimistic-concurrency ---
    // The log is fully client-verified, so the DO stores base64 ops
    // without interpreting them. Append requires the expected seq (=
    // current length) to serialize concurrent writers.
    if (url.pathname === "/log/append" && request.method === "POST") {
      let b;
      try { b = await request.json(); } catch { return json({ error: "bad json" }, 400); }
      const { set, seq, op } = b;
      if (!set || typeof seq !== "number" || typeof op !== "string")
        return json({ error: "missing fields" }, 400);
      const count = (await this.ctx.storage.get(`logcount:${set}`)) ?? 0;
      if (seq !== count) return json({ error: "seq conflict", count }, 409);
      await this.ctx.storage.put(`log:${set}:${seq}`, op);
      await this.ctx.storage.put(`logcount:${set}`, count + 1);
      return json({ ok: true, seq });
    }

    if (url.pathname === "/log/fetch" && request.method === "GET") {
      const set = url.searchParams.get("set") ?? "";
      const count = (await this.ctx.storage.get(`logcount:${set}`)) ?? 0;
      const ops = [];
      // Batch-read the ordered ops.
      const map = await this.ctx.storage.list({ prefix: `log:${set}:` });
      const byseq = [];
      for (const [k, v] of map) byseq.push([parseInt(k.split(":").pop(), 10), v]);
      byseq.sort((a, b) => a[0] - b[0]);
      for (const [, v] of byseq) ops.push(v);
      return json({ count, ops });
    }

    if (url.pathname === "/dir/devices" && request.method === "GET") {
      const out = [];
      for (const [, d] of await this.ctx.storage.list({ prefix: "dev:" }))
        out.push({ name: d.name, fingerprint: d.fingerprint,
                   last_seen: d.last_seen });
      return json({ devices: out });
    }

    // Pairing mailbox: two boxes (offer/answer) keyed by invite id. The
    // secret that authenticates the exchange never touches the DO — it
    // travels in the pasted code, out of band — so this is a dumb relay.
    if (url.pathname === "/pair/put" && request.method === "POST") {
      let b;
      try { b = await request.json(); } catch { return json({ error: "bad json" }, 400); }
      const { id, box, payload } = b;
      if (!id || (box !== "offer" && box !== "answer") || typeof payload !== "string")
        return json({ error: "missing fields" }, 400);
      await this.ctx.storage.put(`pair:${id}:${box}`, { payload, ts: Date.now() });
      return json({ ok: true });
    }
    if (url.pathname === "/pair/get" && request.method === "GET") {
      const id = url.searchParams.get("id") ?? "";
      const box = url.searchParams.get("box") ?? "";
      const v = await this.ctx.storage.get(`pair:${id}:${box}`);
      if (!v || Date.now() - v.ts > 900000) return json({ empty: true });
      return json({ payload: v.payload });
    }

    return json({ error: "not found" }, 404);
  }

  device(ws) {
    return this.ctx.getTags(ws)[0];
  }

  broadcast(obj, exceptDevice) {
    const msg = JSON.stringify(obj);
    for (const ws of this.ctx.getWebSockets())
      if (this.device(ws) !== exceptDevice) ws.send(msg);
  }

  async webSocketMessage(ws, raw) {
    let m;
    try { m = JSON.parse(raw); } catch { return; }
    const from = this.device(ws);

    switch (m.type) {
      case "roster": {
        const online = this.ctx.getWebSockets().map((w) => this.device(w));
        const seen = {};
        for (const [k, v] of await this.ctx.storage.list({ prefix: "seen:" }))
          seen[k.slice(5)] = v;
        ws.send(JSON.stringify({ type: "roster", online, seen }));
        break;
      }
      case "send": {
        // Ferry m.payload to device m.to; this is the signaling path.
        const targets = this.ctx.getWebSockets(m.to);
        if (targets.length === 0) {
          ws.send(JSON.stringify({ type: "error", error: "offline", to: m.to, echo: m.echo }));
          break;
        }
        targets[0].send(JSON.stringify({ type: "msg", from, payload: m.payload, echo: m.echo }));
        break;
      }
      case "turn-creds": {
        const r = await fetch(
          `https://rtc.live.cloudflare.com/v1/turn/keys/${this.env.TURN_KEY_ID}/credentials/generate-ice-servers`,
          {
            method: "POST",
            headers: {
              Authorization: `Bearer ${this.env.TURN_KEY_API_TOKEN}`,
              "Content-Type": "application/json",
            },
            body: JSON.stringify({ ttl: m.ttl ?? 86400 }),
          },
        );
        ws.send(JSON.stringify({ type: "turn-creds", status: r.status, body: await r.json() }));
        break;
      }
      case "whoami": {
        // Diagnostics: lets us observe hibernation wake-ups.
        ws.send(JSON.stringify({ type: "whoami", device: from, echo: m.echo }));
        break;
      }
    }
  }

  async webSocketClose(ws, code, reason, wasClean) {
    const device = this.device(ws);
    await this.ctx.storage.put(`seen:${device}`, Date.now());
    this.broadcast({ type: "left", device, code, wasClean }, device);
    ws.close(1000);
  }

  webSocketError(ws) {
    ws.close(1011);
  }
}
