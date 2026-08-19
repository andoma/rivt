// Rendezvous DO test client (node 22+, native WebSocket).
//
//   node test-client.mjs <wss-base> <token> pair          # two devices, ferry RTT x20, roster, turn-creds
//   node test-client.mjs <wss-base> <token> idle <secs>   # one device, idle (pings only), then whoami
//
// <wss-base> like wss://rivt-rendezvous-spike.<acct>.workers.dev
const [base, token, mode, arg] = process.argv.slice(2);
if (!base || !token || !mode) { console.error("usage: see header"); process.exit(1); }

const url = (dev) => `${base}/ws?set=spike&device=${dev}&token=${token}`;
const now = () => performance.now();

function connect(dev) {
  return new Promise((resolve, reject) => {
    const t0 = now();
    const ws = new WebSocket(url(dev));
    ws.addEventListener("open", () => resolve({ ws, dev, connectMs: now() - t0 }));
    ws.addEventListener("error", (e) => reject(new Error(`${dev}: ${e.message}`)));
  });
}
const send = (c, obj) => c.ws.send(JSON.stringify(obj));
function expect(c, pred, timeoutMs = 10000) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${c.dev}: timeout waiting for message`)), timeoutMs);
    const h = (ev) => {
      const m = JSON.parse(ev.data);
      if (pred(m)) { clearTimeout(timer); c.ws.removeEventListener("message", h); resolve(m); }
    };
    c.ws.addEventListener("message", h);
  });
}

if (mode === "pair") {
  const A = await connect("A");
  const B = await connect("B");
  console.log(`connect: A ${A.connectMs.toFixed(0)}ms, B ${B.connectMs.toFixed(0)}ms`);

  send(A, { type: "roster" });
  const roster = await expect(A, (m) => m.type === "roster");
  console.log("roster:", roster.online.sort().join(","), "seen:", Object.keys(roster.seen).sort().join(","));

  // B echoes ferried messages back.
  B.ws.addEventListener("message", (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === "msg") send(B, { type: "send", to: m.from, payload: m.payload, echo: m.echo });
  });

  const rtts = [];
  for (let i = 0; i < 20; i++) {
    const t0 = now();
    send(A, { type: "send", to: "B", payload: "x".repeat(200), echo: i });
    await expect(A, (m) => m.type === "msg" && m.echo === i);
    rtts.push(now() - t0);
  }
  rtts.sort((a, b) => a - b);
  console.log(`ferry rtt A->DO->B->DO->A: min ${rtts[0].toFixed(1)}ms  median ${rtts[10].toFixed(1)}ms  max ${rtts[19].toFixed(1)}ms`);

  send(A, { type: "turn-creds", ttl: 600 });
  const tc = await expect(A, (m) => m.type === "turn-creds", 15000);
  const turn = tc.body.iceServers?.find((s) => s.username);
  console.log(`turn-creds: http ${tc.status}, username len ${turn?.username?.length ?? "MISSING"}`);
  if (tc.status !== 201) console.log("turn-creds body:", JSON.stringify(tc.body).slice(0, 400));

  send(A, '{"type":"ping"}' === undefined ? {} : { type: "ping" });
  A.ws.send('{"type":"ping"}');
  await expect(A, (m) => m.type === "pong");
  console.log("auto-response ping/pong: ok");
  process.exit(0);
} else if (mode === "idle") {
  const secs = Number(arg ?? 300);
  const C = await connect("idler");
  console.log(`connected in ${C.connectMs.toFixed(0)}ms; idling ${secs}s with pings every 30s`);
  C.ws.addEventListener("close", (ev) => { console.log(`UNEXPECTED close: ${ev.code} ${ev.reason}`); process.exit(1); });
  const iv = setInterval(() => C.ws.send('{"type":"ping"}'), 30000);
  await new Promise((r) => setTimeout(r, secs * 1000));
  clearInterval(iv);
  const t0 = now();
  send(C, { type: "whoami", echo: "wake" });
  await expect(C, (m) => m.type === "whoami");
  console.log(`whoami after ${secs}s idle: ${(now() - t0).toFixed(1)}ms (DO wake included)`);
  process.exit(0);
}
