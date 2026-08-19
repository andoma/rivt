// rivt rendezvous spike: registry + signaling ferry + TURN credential minting.
//
// Spike auth is a shared secret (?token=...). The real design replaces this
// with an Ed25519 challenge signed by the device key.
//
// Secrets (wrangler secret put): SPIKE_TOKEN, TURN_KEY_ID, TURN_KEY_API_TOKEN

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/ws") {
      const set = url.searchParams.get("set");
      const token = url.searchParams.get("token");
      if (!set || token !== env.SPIKE_TOKEN)
        return new Response("forbidden", { status: 403 });
      const id = env.RENDEZVOUS.idFromName(set);
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
