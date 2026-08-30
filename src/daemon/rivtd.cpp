// rivtd — session daemon. Owns PTYs and authoritative terminal state;
// clients attach over a unix socket using the proto framing
// (channel 0 = control, channel N = pane N's byte stream).
//
// Object lifetimes follow the same rule as the UI: nothing is destroyed
// from inside its own fd callback. Deaths are recorded and swept after
// each poll iteration.
#include "core/config.h"
#include "core/debug.h"
#include "core/event_loop.h"
#include "core/layout.h"
#include "core/pane.h"
#include "net/identity.h"
#include "net/quic_engine.h"
#include "net/membership.h"
#include "net/pairing.h"
#include "net/rendezvous.h"
#include "net/signaling.h"
#include "net/turn.h"
#include <arpa/inet.h>
#include "proto/frame.h"
#include "proto/messages.h"
#include "proto/snapshot.h"
#include "proto/wire.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <ctime>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

namespace rivt {

using proto::MsgType;

static constexpr size_t CLIENT_OUT_MAX = 8 << 20;  // kill client past this
// Attach ships the screen plus this much recent history in one frame;
// the client fetches deeper scrollback on demand (request_scrollback).
// Keep it small: the snapshot is frame-atomic, so nothing renders until
// it fully arrives — 2000 heavy lines took a minute on a lossy link
// while the pane sat blank.
static constexpr int ATTACH_SCROLLBACK_LINES = 100;

struct Client {
    int fd = -1;                          // unix transport, or...
    net::QuicEngine::Conn *quic = nullptr;  // ...QUIC transport
    // Resize coalescing: bursts apply once, after the batch drains.
    bool resize_pending = false;
    int resize_cols = 0, resize_rows = 0;
    // Per-QUIC-stream reassembly buffers (unix transport uses in[0]).
    std::unordered_map<uint64_t, std::string> in;
    // Server-allocated pane streams for this connection (ids 1,5,9,...).
    std::unordered_map<uint16_t, uint64_t> pane_stream;
    uint64_t next_stream = 1;
    std::string out;
    size_t out_off = 0;
    uint32_t attached = 0;  // session id, 0 = not attached
    bool hello = false;
    bool dead = false;
    bool write_armed = false;
};

// One window = one tab client-side. Layout is per window, in cell units
// (1x1 cells, 1-cell dividers).
struct SrvWindow {
    uint32_t id;
    LayoutTree layout;
    std::vector<std::pair<uint16_t, std::unique_ptr<Pane>>> panes;
};

struct Session {
    uint32_t id;
    std::string name;
    int cols = 80, rows = 24;   // session grid; every window fills it
    std::vector<SrvWindow> windows;
    // SSH agent forwarding: shells get SSH_AUTH_SOCK=agent_path; each
    // connection becomes a stream bridged to the attached client.
    int agent_fd = -1;
    std::string agent_path;

    size_t pane_count() const {
        size_t n = 0;
        for (const auto &w : windows) n += w.panes.size();
        return n;
    }
};

struct PaneRef {
    uint32_t sid;
    uint32_t wid;
    Pane *pane;
    // Input-echo acking (predictive echo): last input seq written to the
    // PTY, which client sent it, and what has been acked back to it.
    uint32_t in_seq = 0;
    uint32_t acked_seq = 0;
    Client *in_client = nullptr;
    bool echo_off = false;  // PTY termios ECHO state as last observed
    // Recent output with absolute offsets: a re-attaching client that is
    // current through an offset inside the ring gets the gap replayed
    // instead of a full snapshot (seamless reconnect).
    std::string ring;
    uint64_t ring_end = 0;  // absolute offset of the byte after ring
};

static constexpr size_t PANE_RING_MAX = 256 * 1024;

static constexpr uint32_t HANDOVER_VERSION = 1;

class Daemon {
    // Agent forwarding streams: one per accepted connection on a
    // session's agent socket, pinned to the client chosen at accept.
    // Declared up front: used as a parameter type by member functions,
    // which is not a complete-class context.
    struct AgentStream {
        int fd = -1;
        uint32_t sid = 0;
        Client *client = nullptr;  // valid until sweep() reaps it
        std::string out;
        size_t out_off = 0;
        bool write_armed = false;
    };

public:
    Daemon(std::string socket_path, std::string handover_path, int listen_port,
           std::string device_name)
        : m_path(std::move(socket_path)), m_handover_in(std::move(handover_path)),
          m_listen_port(listen_port), m_name(std::move(device_name)) {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) { exe[n] = 0; m_exe = exe; }
    }

    bool init() {
        if (!m_handover_in.empty()) restore_handover(m_handover_in);
        // Restored shells inherited SSH_AUTH_SOCK from the previous
        // daemon; the path is deterministic per session id, so re-listen
        // on the same paths and their env stays valid across upgrades.
        for (auto &[sid, sess] : m_sessions) session_agent_init(sess);
        // Signals via signalfd: SIGCHLD for reaping, SIGTERM/SIGINT to quit.
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGPIPE);
        sigaddset(&mask, SIGUSR1);  // upgrade: re-exec with sessions kept
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        m_sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (m_sig_fd < 0) { perror("signalfd"); return false; }
        m_loop.add_fd(m_sig_fd, [this](uint32_t) { on_signal(); });

        if (!setup_socket()) return false;
        m_loop.add_fd(m_listen_fd, [this](uint32_t) { on_accept(); });

        if (m_listen_port > 0) {
            m_identity = net::Identity::load_or_create();
            if (!m_identity) { rivt::logmsg("rivtd: cannot create identity\n"); return false; }
            // Membership drives trust: load the set (auto-init a
            // single-device set if none), sync it, and write the QUIC
            // bundle from it — before the engine reads the bundle.
            sync_membership();
            std::string bundle = net::Identity::authorized_bundle_path();
            m_quic = net::QuicEngine::listen(m_loop, (uint16_t)m_listen_port,
                                             *m_identity, bundle);
            if (!m_quic) return false;
            rivt::logmsg(                    "rivtd: QUIC on udp/%d\n"
                    "rivtd: fingerprint %s\n"
                    "rivtd: authorized peers: %s\n",
                    m_listen_port, m_identity->fingerprint().c_str(), bundle.c_str());
            m_quic->on_connected = [this](net::QuicEngine::Conn *conn) {
                auto c = std::make_unique<Client>();
                c->quic = conn;
                conn->user = c.get();
                m_clients.push_back(std::move(c));
            };
            m_quic->on_data = [this](net::QuicEngine::Conn *conn, uint64_t stream,
                                     const uint8_t *d, size_t n) {
                Client *c = (Client *)conn->user;
                if (!c || c->dead) return;
                c->in[stream].append((const char *)d, n);
                process_client(c, stream);
            };
            m_quic->on_closed = [this](net::QuicEngine::Conn *conn) {
                Client *c = (Client *)conn->user;
                if (c) {
                    conn->user = nullptr;
                    c->quic = nullptr;
                    resume_session_panes(c->attached);  // never leave panes paused
                    kill_client(c);
                }
            };
            m_quic->on_drained = [this](net::QuicEngine::Conn *conn) {
                Client *c = (Client *)conn->user;
                if (c) resume_session_panes(c->attached);
            };

            // Publish to the device directory, if configured. Blocking
            // HTTPS runs in a short-lived child so sessions never stall.
            std::string rdv = net::rendezvous_url();
            if (!rdv.empty()) {
                auto publish = [this, rdv]() {
                    pid_t pid = fork();
                    if (pid != 0) return;
                    net::register_device(rdv, *m_identity, m_name,
                                         (uint16_t)m_listen_port);
                    _exit(0);
                };
                rivt::logmsg("rivtd: publishing '%s' to %s\n", m_name.c_str(),
                        rdv.c_str());
                publish();
                m_loop.add_timer(60000, publish, true);
                // Re-sync the set periodically so newly-paired devices
                // become trusted without a restart. Forked like publish():
                // it's blocking HTTPS, and the daemon must never wait on
                // the network — the child writes the bundle file, which
                // QUIC re-reads from disk per handshake anyway.
                m_loop.add_timer(60000, [this]() {
                    pid_t pid = fork();
                    if (pid != 0) return;
                    sync_membership();
                    _exit(0);
                }, true);

                // Persistent signaling channel so clients behind NAT can
                // summon us for a hole punch / relay.
                start_signaling(rdv);

                // Relay data-path warming, liveness, and retired-relay
                // reaping (see turn_self_probe / turn_maintenance).
                m_loop.add_timer(60000, [this]() { turn_maintenance(); }, true);
            }
        }
        return true;
    }

    void start_signaling(const std::string &rdv) {
        m_signaling = std::make_unique<net::Signaling>(m_loop, *m_identity);
        m_signaling->on_candidates =
            [this](const std::string &from, bool answer, std::vector<net::Candidate> cands) {
                if (!answer) handle_offer(from, cands);
            };
        m_signaling->on_ready = []() {
            rivt::logmsg("rivtd: signaling connected (ready for hole punch / relay)\n");
        };
        // Reconnect on detectable transport loss (edge idle-close, RST).
        // Deferred to a timer: restarting the WS from inside its own
        // close callback would re-enter the client.
        m_signaling->on_close = [this]() {
            rivt::logmsg("rivtd: signaling lost, reconnecting in 2s\n");
            m_loop.add_timer(2000, [this]() {
                if (m_signaling && !m_signaling->ready()) m_signaling->restart();
            }, false);
        };
        if (!m_signaling->start(rdv)) {
            rivt::logmsg("rivtd: signaling failed to start\n");
            m_signaling.reset();
            return;
        }
        // App-level keepalive: edge-answered, keeps the NAT/TCP mapping
        // alive without waking the DO. The edge answers every ping with a
        // pong, so a healthy link receives at least one frame per tick;
        // silence across three ticks means the TCP path died without
        // telling us (dropped NAT mapping, DO eviction) — reconnect.
        m_loop.add_timer(30000, [this]() {
            if (!m_signaling) return;
            double idle = m_signaling->seconds_since_rx();
            if (!m_signaling->ready() || idle > 95.0) {
                rivt::logmsg("rivtd: signaling stale (open=%d, last rx %.0fs ago), "
                        "reconnecting\n", m_signaling->ready(), idle);
                m_signaling->restart();
            } else {
                m_signaling->keepalive();
            }
        }, true);
    }

    // A client wants to reach us: STUN our listen socket, allocate a TURN
    // relay + permit the client, answer with our candidates, and punch.
    void handle_offer(const std::string &from, const std::vector<net::Candidate> &client_cands) {
        dbg("rivtd: punch offer from %.16s... (%zu candidates)\n",
                from.c_str(), client_cands.size());
        // Rotate the advertised relay when: none yet, the current one is
        // dead (refresh failure or probe silence), or it has served
        // enough answers that its silent ~10-peer-flow budget is at
        // risk. Old relays stay in m_relays serving established
        // sessions until reaped (see turn_maintenance).
        bool need_new = !m_turn;
        if (m_turn) {
            bool data_dead = m_turn->probing() && m_turn->seconds_since_data() > 150.0;
            if (!m_turn->alive() || data_dead) {
                rivt::logmsg("rivtd: current relay %s:%u is dead (%s), rotating\n",
                             m_turn->relayed_host().c_str(), m_turn->relayed_port(),
                             m_turn->alive() ? "data path silent" : "refresh failed");
                need_new = true;
            } else if (m_turn_answers >= 4) {
                rivt::logmsg("rivtd: relay %s:%u served %d answers — rotating "
                             "(peer-flow budget)\n",
                             m_turn->relayed_host().c_str(), m_turn->relayed_port(),
                             m_turn_answers);
                need_new = true;
            }
        }
        if (need_new) allocate_relay();
        if (m_turn) m_turn_answers++;
        // Permit and punch toward every client candidate.
        for (const auto &c : client_cands) {
            struct sockaddr_in sa {};
            bool permitted = false;
            if (inet_pton(AF_INET, c.host.c_str(), &sa.sin_addr) == 1) {
                sa.sin_family = AF_INET;
                sa.sin_port = htons(c.port);
                if (m_turn) { m_turn->permit(sa); permitted = true; }
            }
            dbg("rivtd:   punching [%-8s] %s:%u%s\n", c.kind.c_str(),
                    c.host.c_str(), c.port, permitted ? " (relay permit)" : "");
            m_quic->punch(c.host, c.port);
        }
        // Keep punching briefly to cover handshake timing.
        auto punches = std::make_shared<int>(0);
        int tid = m_loop.add_timer(200, [this, client_cands, punches]() {
            for (const auto &c : client_cands) m_quic->punch(c.host, c.port);
            (*punches)++;
        }, true);
        m_loop.add_timer(2500, [this, tid]() { m_loop.remove_timer(tid); }, false);

        // STUN our listen socket, then answer with local+stun+turn.
        m_quic->discover_reflexive([this, from](bool ok, const struct sockaddr_storage &sa) {
            std::vector<net::Candidate> mine;
            for (const auto &a : net::local_addresses())
                mine.push_back({a, (uint16_t)m_listen_port, "local"});
            if (ok) {
                char ip[64] = {0};
                uint16_t port = 0;
                if (sa.ss_family == AF_INET) {
                    auto *s = (const struct sockaddr_in *)&sa;
                    inet_ntop(AF_INET, &s->sin_addr, ip, sizeof ip); port = ntohs(s->sin_port);
                    m_reflexive = *s;
                } else {
                    auto *s = (const struct sockaddr_in6 *)&sa;
                    inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof ip); port = ntohs(s->sin6_port);
                    if (IN6_IS_ADDR_V4MAPPED(&s->sin6_addr)) {
                        m_reflexive.sin_family = AF_INET;
                        m_reflexive.sin_port = s->sin6_port;
                        memcpy(&m_reflexive.sin_addr, &s->sin6_addr.s6_addr[12], 4);
                    }
                }
                mine.push_back({ip, port, "stun"});
                turn_self_probe();  // first data-path probe + arm the watchdog
            }
            if (m_turn && m_turn->alive())
                mine.push_back({m_turn->relayed_host(), m_turn->relayed_port(), "turn"});
            dbg("rivtd: answering %.16s... with %zu candidate(s):\n",
                    from.c_str(), mine.size());
            for (const auto &c : mine)
                dbg("rivtd:   [%-8s] %s:%u\n", c.kind.c_str(), c.host.c_str(), c.port);
            if (m_signaling) m_signaling->send(from, /*answer=*/true, mine);
        });
    }

    // Push one packet of real peer traffic through our own relayed
    // address: permit our reflexive, punch the relayed address from the
    // QUIC socket. This keeps Cloudflare's relayed port warm (an idle
    // one stops forwarding after ~30-40 min even though refreshes keep
    // succeeding) and gives TurnRelay a hard liveness signal — the
    // probe must come back as a Data indication.
    void turn_self_probe() {
        if (!m_turn || !m_turn->alive() || !m_quic) return;
        if (m_reflexive.sin_family != AF_INET) return;
        m_turn->permit(m_reflexive);  // per-IP, deduped after the first
        m_quic->punch(m_turn->relayed_host(), m_turn->relayed_port());
        m_turn->expect_probes();
    }

    // Fetch (and cache) TURN credentials, allocate a fresh relay, make
    // it current. Old relays are left in m_relays for their sessions.
    void allocate_relay() {
        if (m_cred_user.empty() || time(nullptr) - m_cred_fetched > 6 * 3600) {
            if (!net::turn_credentials(net::rendezvous_url(), m_cred_user, m_cred_pass,
                                       m_cred_host, m_cred_port)) {
                rivt::logmsg("rivtd: turn credential fetch failed\n");
                m_cred_user.clear();
                return;
            }
            m_cred_fetched = time(nullptr);
        }
        auto t = std::make_unique<net::TurnRelay>(m_loop);
        if (!t->allocate(m_cred_host, m_cred_port, m_cred_user, m_cred_pass)) {
            rivt::logmsg("rivtd: turn allocation failed — answering without relay\n");
            m_turn = nullptr;
            return;
        }
        m_turn = t.get();
        m_turn_answers = 0;
        m_quic->add_turn(m_turn);
        m_relays.push_back(std::move(t));
        turn_self_probe();  // warm + arm the new relay immediately
    }

    // Reap retired relays: not current, and silent long enough that no
    // established session still runs through them (session keepalives
    // arrive at least every 30s). Dead ones go faster. With no QUIC
    // clients at all for a few minutes, release everything — keeping an
    // allocation warm buys nothing (a fresh one takes ~30ms inside the
    // answer flow) and just generates refresh/probe traffic forever.
    void turn_maintenance() {
        bool have_clients = false;
        for (const auto &c : m_clients)
            if (!c->dead && c->quic) { have_clients = true; break; }
        if (!have_clients && m_turn) {
            if (++m_turn_idle_ticks >= 5) {
                rivt::logmsg("rivtd: no clients for %d min — releasing %zu turn "
                             "relay(s)\n", m_turn_idle_ticks, m_relays.size());
                for (auto &t : m_relays) m_quic->remove_turn(t.get());
                m_relays.clear();
                m_turn = nullptr;
                m_turn_idle_ticks = 0;
                return;
            }
        } else {
            m_turn_idle_ticks = 0;
        }
        turn_self_probe();
        std::erase_if(m_relays, [this](std::unique_ptr<net::TurnRelay> &t) {
            if (t.get() == m_turn) return false;
            double quiet = t->seconds_since_data();
            bool reap = (!t->alive() && quiet > 120.0) || quiet > 600.0;
            if (reap) {
                rivt::logmsg("rivtd: reaping retired relay %s:%u (silent %.0fs)\n",
                             t->relayed_host().c_str(), t->relayed_port(), quiet);
                m_quic->remove_turn(t.get());
            }
            return reap;
        });
    }

    // Load the membership log (auto-init a single-device set if none),
    // reconcile with the rendezvous, and rewrite the QUIC trust bundle.
    // Live QUIC verification re-reads the bundle per handshake via the
    // TLS store, so a rewrite takes effect for new connections.
    void sync_membership() {
        net::sync_membership(*m_identity, /*found_if_missing=*/true);
    }

    int run() {
        while (m_loop.poll(-1)) sweep();
        return 0;
    }

private:
    bool setup_socket() {
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (m_path.size() >= sizeof(addr.sun_path)) {
            rivt::logmsg("socket path too long: %s\n", m_path.c_str());
            return false;
        }
        strcpy(addr.sun_path, m_path.c_str());

        m_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0) { perror("socket"); return false; }

        if (bind(m_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (errno != EADDRINUSE) { perror("bind"); return false; }
            // Stale socket? If nobody answers, take over.
            int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            bool live = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            close(probe);
            if (live) {
                rivt::logmsg("rivtd already running on %s\n", m_path.c_str());
                return false;
            }
            unlink(m_path.c_str());
            if (bind(m_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                perror("bind");
                return false;
            }
        }
        chmod(m_path.c_str(), 0600);
        if (listen(m_listen_fd, 8) < 0) { perror("listen"); return false; }
        return true;
    }

    void on_signal() {
        struct signalfd_siginfo si;
        while (read(m_sig_fd, &si, sizeof si) == sizeof si) {
            if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
                m_loop.request_quit();
            if (si.ssi_signo == SIGUSR1)
                upgrade();
            if (si.ssi_signo == SIGCHLD) {
                int status;
                pid_t pid;
                while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                    // How a pane's shell died is the difference between
                    // "user logged out" and "something killed it" — log
                    // it. Non-pane children (publish forks) stay silent.
                    for (const auto &[pane_id, ref] : m_panes) {
                        if (ref.pane->pty().child_pid() != pid) continue;
                        if (WIFEXITED(status))
                            rivt::logmsg("rivtd: pane %u shell (pid %d) exited "
                                         "with status %d\n",
                                         pane_id, pid, WEXITSTATUS(status));
                        else if (WIFSIGNALED(status))
                            rivt::logmsg("rivtd: pane %u shell (pid %d) killed "
                                         "by signal %d\n",
                                         pane_id, pid, WTERMSIG(status));
                        break;
                    }
                }
            }
        }
    }

    // ---------------- client I/O ----------------

    void on_accept() {
        int fd;
        while ((fd = accept4(m_listen_fd, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
            auto c = std::make_unique<Client>();
            c->fd = fd;
            Client *raw = c.get();
            m_clients.push_back(std::move(c));
            m_loop.add_fd(fd, [this, raw](uint32_t ev) { client_event(raw, ev); });
        }
    }

    void client_event(Client *c, uint32_t ev) {
        if (c->dead) return;
        if (ev & EV_WRITE) flush_client(c);
        // Drain and process before honoring HUP/EOF: a client's final
        // frames (e.g. KillSession right before window close) arrive in
        // the same event batch as the hangup.
        bool eof = false;
        if (ev & (EV_READ | EV_HUP)) {
            char buf[65536];
            for (;;) {
                ssize_t n = recv(c->fd, buf, sizeof buf, 0);
                if (n > 0) { c->in[0].append(buf, n); continue; }
                if (n == 0) { eof = true; break; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                eof = true;
                break;
            }
            process_client(c, 0);
        }
        if (eof || (ev & (EV_HUP | EV_ERR))) kill_client(c);
    }

    void process_client(Client *c, uint64_t stream) {
        std::string &in = c->in[stream];
        while (!c->dead && in.size() >= proto::FRAME_HEADER_SIZE) {
            proto::FrameHeader h;
            if (!proto::decode_frame_header((const uint8_t *)in.data(), h)) {
                kill_client(c);
                return;
            }
            size_t total = proto::FRAME_HEADER_SIZE + h.len;
            if (in.size() < total) return;
            const uint8_t *payload = (const uint8_t *)in.data() + proto::FRAME_HEADER_SIZE;
            if (h.channel == 0)
                handle_control(c, (MsgType)h.type, payload, h.len);
            else if (h.type == proto::PANE_IN)
                handle_pane_input(c, h.channel, payload, h.len);
            in.erase(0, total);
        }
        if (!c->dead && c->resize_pending) {
            c->resize_pending = false;
            auto it = m_sessions.find(c->attached);
            if (it != m_sessions.end() &&
                (it->second.cols != c->resize_cols || it->second.rows != c->resize_rows)) {
                it->second.cols = c->resize_cols;
                it->second.rows = c->resize_rows;
                for (auto &win : it->second.windows) relayout(it->second, win);
            }
        }
    }

    void send_frame(Client *c, uint16_t channel, uint16_t type,
                    const void *data, size_t len) {
        if (c->dead) return;
        if (c->quic) {
            // Control on stream 0; each pane on its own server-initiated
            // stream so bulk output can't head-of-line-block the rest.
            uint64_t stream = 0;
            if (channel != 0) {
                auto [it, fresh] = c->pane_stream.try_emplace(channel, 0);
                if (fresh) {
                    it->second = c->next_stream;
                    c->next_stream += 4;
                }
                stream = it->second;
            }
            uint8_t hdr[proto::FRAME_HEADER_SIZE];
            proto::encode_frame_header(hdr, {(uint32_t)len, channel, type});
            m_quic->send(c->quic, stream, hdr, sizeof hdr);
            m_quic->send(c->quic, stream, data, len);
            return;
        }
        if (c->out.size() - c->out_off + len > CLIENT_OUT_MAX) {
            // Slow/stuck client. Design says snapshot-resync; v1 policy
            // is disconnect (the client reconnects and re-attaches).
            kill_client(c);
            return;
        }
        uint8_t hdr[proto::FRAME_HEADER_SIZE];
        proto::encode_frame_header(hdr, {(uint32_t)len, channel, type});
        c->out.append((const char *)hdr, sizeof hdr);
        c->out.append((const char *)data, len);
        flush_client(c);
    }

    void send_control(Client *c, MsgType t, const proto::Writer &w) {
        send_frame(c, 0, (uint16_t)t, w.buf.data(), w.buf.size());
    }

    void flush_client(Client *c) {
        if (c->quic) return;  // picoquic buffers internally
        while (c->out_off < c->out.size()) {
            ssize_t n = send(c->fd, c->out.data() + c->out_off,
                             c->out.size() - c->out_off, MSG_NOSIGNAL);
            if (n > 0) { c->out_off += n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            kill_client(c);
            return;
        }
        if (c->out_off == c->out.size()) {
            c->out.clear();
            c->out_off = 0;
        }
        bool want_write = c->out_off < c->out.size();
        if (want_write != c->write_armed) {
            c->write_armed = want_write;
            m_loop.modify_fd(c->fd, want_write ? (EV_READ | EV_WRITE) : EV_READ);
        }
    }

    void kill_client(Client *c) {
        if (c->dead) return;
        rivt::logmsg("rivtd: client disconnected (attached session %u)\n", c->attached);
        c->dead = true;
        m_sweep_clients = true;
    }

    // ---------------- control handling ----------------

    void handle_control(Client *c, MsgType t, const uint8_t *data, size_t len) {
        proto::Reader r(data, len);
        if (t == MsgType::UpgradeDaemon) {
            upgrade();
            return;
        }
        if (!c->hello) {
            uint32_t ver = (t == MsgType::Hello) ? r.u32() : 0;
            if (t != MsgType::Hello || !r.ok || ver != proto::PROTO_VERSION) {
                char msg[128];
                snprintf(msg, sizeof msg,
                         "protocol version mismatch (daemon %u, client %u) — "
                         "if you upgraded rivt, restart the daemon: pkill -x rivtd",
                         proto::PROTO_VERSION, ver);
                send_error(c, msg);
                flush_client(c);
                kill_client(c);
                return;
            }
            c->hello = true;
            proto::Writer w;
            w.u32(proto::PROTO_VERSION);
            // Daemon epoch: pane output offsets are only comparable
            // within one daemon lifetime; a client that reconnects to a
            // restarted daemon must snapshot, never resume.
            w.u32(m_epoch);
            send_control(c, MsgType::HelloOk, w);
            return;
        }

        switch (t) {
        case MsgType::ListSessions: {
            proto::Writer w;
            w.u32((uint32_t)m_sessions.size());
            for (auto &[id, s] : m_sessions) {
                w.u32(id);
                w.str(s.name);
                w.u32((uint32_t)s.pane_count());
            }
            send_control(c, MsgType::SessionList, w);
            break;
        }
        case MsgType::CreateSession: {
            std::string name = r.str(), cwd = r.str();
            int cols = r.u16(), rows = r.u16();
            if (!r.ok || cols < 2 || rows < 2 || cols > 4096 || rows > 4096) {
                kill_client(c);
                return;
            }
            create_session(c, name, cwd, cols, rows);
            break;
        }
        case MsgType::Attach: {
            uint32_t sid = r.u32();
            std::unordered_map<uint32_t, uint64_t> resume;
            while (r.ok && r.remaining() >= 12) {
                uint32_t pid = r.u32();
                uint64_t off = r.u64();
                if (r.ok) resume[pid] = off;
            }
            auto it = m_sessions.find(sid);
            if (!r.ok || it == m_sessions.end()) {
                send_error(c, "no such session");
                return;
            }
            c->attached = sid;
            Session &s = it->second;
            proto::Writer w;
            w.u32(sid);
            send_control(c, MsgType::AttachOk, w);
            for (auto &win : s.windows) {
                proto::Writer wa;
                wa.u32(sid);
                wa.u32(win.id);
                send_control(c, MsgType::WindowAdded, wa);
                send_layout_to(c, s, win);
            }
            for (auto &win : s.windows)
                for (auto &[pid, pane] : win.panes) {
                    // Seamless path: the client is current through an
                    // offset still in the ring — replay only the gap.
                    auto pref = m_panes.find(pid);
                    auto res = resume.find(pid);
                    if (pref != m_panes.end() && res != resume.end()) {
                        const PaneRef &ref = pref->second;
                        uint64_t ring_start = ref.ring_end - ref.ring.size();
                        if (res->second >= ring_start && res->second <= ref.ring_end) {
                            uint64_t off = res->second;
                            send_frame(c, pid, proto::PANE_RESUME, &off, 8);
                            size_t skip = (size_t)(off - ring_start);
                            if (ref.ring.size() > skip)
                                send_frame(c, pid, proto::PANE_OUT,
                                           ref.ring.data() + skip,
                                           ref.ring.size() - skip);
                            dbg("rivtd: pane %u resumed at %llu (+%zu replay)",
                                pid, (unsigned long long)off, ref.ring.size() - skip);
                            continue;
                        }
                    }
                    auto blob = proto::Snapshot::serialize(pane->screen(), pane->parser(),
                                                           ATTACH_SCROLLBACK_LINES);
                    if (blob.size() > proto::FRAME_MAX_LEN / 2) {
                        // Heavy scrollback: ship less history rather than
                        // an oversized frame (rest is fetchable on scroll).
                        rivt::logmsg("rivtd: pane %u snapshot %zu bytes — "
                                     "trimming to 200 lines\n", pid, blob.size());
                        blob = proto::Snapshot::serialize(pane->screen(), pane->parser(),
                                                          200);
                    }
                    send_frame(c, pid, proto::PANE_SNAPSHOT, blob.data(), blob.size());
                    // Anchor the client's offset counting at now.
                    if (pref != m_panes.end()) {
                        uint64_t off = pref->second.ring_end;
                        send_frame(c, pid, proto::PANE_RESUME, &off, 8);
                    }
                }
            break;
        }
        case MsgType::Detach:
            c->attached = 0;
            break;
        case MsgType::Resize: {
            int cols = r.u16(), rows = r.u16();
            if (!r.ok || cols < 2 || rows < 2 || cols > 4096 || rows > 4096) return;
            // Deferred: a drag can deliver dozens of resizes in one read
            // batch, and each apply reflows every pane's scrollback.
            // Only the last one matters.
            c->resize_pending = true;
            c->resize_cols = cols;
            c->resize_rows = rows;
            break;
        }
        case MsgType::Split: {
            uint32_t pid = r.u32();
            uint8_t dir = r.u8();
            auto pit = m_panes.find((uint16_t)pid);
            if (!r.ok || pit == m_panes.end() || pit->second.sid != c->attached) return;
            split_pane(m_sessions.at(c->attached), pit->second.wid, pit->second.pane,
                       dir == 0 ? SplitDir::Vertical : SplitDir::Horizontal);
            break;
        }
        case MsgType::ClosePane: {
            uint32_t pid = r.u32();
            auto pit = m_panes.find((uint16_t)pid);
            if (!r.ok || pit == m_panes.end() || pit->second.sid != c->attached) return;
            // Explicit removal: closing our master fd delivers no HUP to
            // us, so the pane-death path would never fire. We're inside a
            // client callback (not the pane's), so immediate teardown is
            // safe. Detach before close so the fd leaves the loop first.
            Pane *pane = pit->second.pane;
            pane->detach(m_loop);
            pane->pty().close();
            m_panes.erase(pit);
            remove_dead_pane((uint16_t)pid);
            break;
        }
        case MsgType::FetchScrollback: {
            uint32_t pid = r.u32();
            uint32_t end_abs = r.u32();
            uint32_t count = r.u32();
            auto pit = m_panes.find((uint16_t)pid);
            if (!r.ok || pit == m_panes.end() || pit->second.sid != c->attached) return;
            if (count > 1000) count = 1000;

            const ScreenBuffer &sb = pit->second.pane->screen();
            uint32_t lo = (uint32_t)sb.scrollback_trimmed();  // oldest we still hold
            uint32_t hi = lo + (uint32_t)sb.scrollback_count();
            uint32_t end = end_abs < lo ? lo : (end_abs > hi ? hi : end_abs);
            uint32_t start = end - lo > count ? end - count : lo;

            proto::Writer w;
            w.u32(start);
            w.u32(end - start);
            for (uint32_t a = start; a < end; a++) {
                // scrollback_line(0) is the most recent line
                int idx = sb.scrollback_count() - 1 - (int)(a - lo);
                proto::Snapshot::encode_line(w, sb.scrollback_line(idx));
            }
            send_frame(c, (uint16_t)pid, proto::PANE_SCROLLBACK, w.buf.data(), w.buf.size());
            break;
        }
        case MsgType::NewWindow: {
            auto it = m_sessions.find(c->attached);
            if (it == m_sessions.end()) return;
            add_window(it->second, "");
            break;
        }
        case MsgType::CloseWindow: {
            uint32_t wid = r.u32();
            auto it = m_sessions.find(c->attached);
            if (!r.ok || it == m_sessions.end()) return;
            close_window(it->second, wid);
            break;
        }
        case MsgType::KillSession: {
            uint32_t sid = r.u32();
            auto it = m_sessions.find(sid);
            if (!r.ok || it == m_sessions.end()) return;
            rivt::logmsg("rivtd: KillSession %u from client\n", sid);
            close_session(it->second, "kill requested by client");
            m_sessions.erase(it);
            break;
        }
        case MsgType::ResizePane: {
            uint32_t pid = r.u32();
            int32_t dx = r.i32();
            int32_t dy = r.i32();
            if (!r.ok || (!dx && !dy)) return;
            auto it = m_panes.find((uint16_t)pid);
            if (it == m_panes.end() || it->second.sid != c->attached) return;
            Session &s = m_sessions.at(it->second.sid);
            for (auto &win : s.windows) {
                if (win.id != it->second.wid) continue;
                // Window layout is in cell units, so deltas are direct.
                bool changed = false;
                if (dx) changed |= win.layout.resize_edge(it->second.pane, true, dx);
                if (dy) changed |= win.layout.resize_edge(it->second.pane, false, dy);
                if (changed) relayout(s, win);
                break;
            }
            break;
        }
        case MsgType::AgentData: {
            uint32_t id = r.u32();
            if (!r.ok) return;
            auto it = m_agent_streams.find(id);
            if (it == m_agent_streams.end() || it->second.client != c) return;
            size_t n = r.remaining();
            it->second.out.append((const char *)data + (len - n), n);
            agent_stream_flush(it->second);
            break;
        }
        case MsgType::AgentClose: {
            uint32_t id = r.u32();
            if (!r.ok) return;
            auto it = m_agent_streams.find(id);
            if (it != m_agent_streams.end() && it->second.client == c)
                close_agent_stream(id, false);
            break;
        }
        default:
            break;
        }
    }

    void handle_pane_input(Client *c, uint16_t pane_id, const uint8_t *data, size_t len) {
        if (!c->hello) { kill_client(c); return; }
        if (len < 4) return;  // v5: u32 seq prefixes the bytes
        auto it = m_panes.find(pane_id);
        if (it == m_panes.end() || it->second.sid != c->attached) return;
        uint32_t seq;
        memcpy(&seq, data, 4);
        it->second.in_seq = seq;
        it->second.in_client = c;
        it->second.pane->write((const char *)data + 4, len - 4);
    }

    void send_error(Client *c, const char *msg) {
        proto::Writer w;
        w.str(msg);
        send_control(c, MsgType::Error, w);
    }

    // ---------------- sessions ----------------

    void create_session(Client *c, const std::string &name, const std::string &cwd,
                        int cols, int rows) {
        uint32_t sid = m_next_sid++;
        Session s;
        s.id = sid;
        s.name = name.empty() ? ("session-" + std::to_string(sid)) : name;
        s.cols = cols;
        s.rows = rows;
        m_sessions.emplace(sid, std::move(s));
        session_agent_init(m_sessions.at(sid));

        uint16_t pid = add_window(m_sessions.at(sid), cwd);
        if (!pid) {
            m_sessions.erase(sid);
            proto::Writer w;
            w.u32(0);
            w.u32(0);
            w.str("failed to spawn shell");
            send_control(c, MsgType::SessionCreated, w);
            return;
        }

        proto::Writer w;
        w.u32(sid);
        w.u32(pid);
        w.str("");
        send_control(c, MsgType::SessionCreated, w);
    }

    // Create a window with one shell pane; announces WindowAdded and the
    // window's layout to attached clients. Returns the pane id, 0 on failure.
    uint16_t add_window(Session &s, const std::string &cwd) {
        uint16_t pid = m_next_pane++;
        auto pane = std::make_unique<Pane>(s.cols, s.rows, m_config);
        if (!pane->spawn_shell(m_loop, cwd, s.agent_path)) return 0;

        SrvWindow win;
        win.id = m_next_wid++;
        win.layout.init(pane.get());
        wire_pane(s.id, pid, pane.get());
        win.panes.emplace_back(pid, std::move(pane));
        m_panes[pid] = {s.id, win.id, win.panes.back().second.get()};
        s.windows.push_back(std::move(win));

        proto::Writer w;
        w.u32(s.id);
        w.u32(s.windows.back().id);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id)
                send_control(c.get(), MsgType::WindowAdded, w);
        relayout(s, s.windows.back());
        return pid;
    }

    void close_window(Session &s, uint32_t wid) {
        auto wit = std::find_if(s.windows.begin(), s.windows.end(),
                                [wid](const SrvWindow &w) { return w.id == wid; });
        if (wit == s.windows.end()) return;
        rivt::logmsg("rivtd: window %u closed (session %u, %zu window(s) remain)\n",
                     wid, s.id, s.windows.size() - 1);
        for (auto &[pid, pane] : wit->panes) {
            pane->detach(m_loop);
            pane->pty().close();
            m_panes.erase(pid);
        }
        proto::Writer w;
        w.u32(s.id);
        w.u32(wid);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id)
                send_control(c.get(), MsgType::WindowClosed, w);
        s.windows.erase(wit);
        if (s.windows.empty()) {
            uint32_t sid = s.id;
            close_session(s, "last window closed");
            m_sessions.erase(sid);
        }
    }

    void wire_pane(uint32_t sid, uint16_t pid, Pane *pane) {
        pane->on_output = [this, sid, pid, pane](const char *d, size_t n) {
            auto rit = m_panes.find(pid);
            if (rit != m_panes.end()) {
                PaneRef &ref = rit->second;
                ref.ring.append(d, n);
                ref.ring_end += n;
                if (ref.ring.size() > PANE_RING_MAX)
                    ref.ring.erase(0, ref.ring.size() - PANE_RING_MAX);
            }
            for (auto &c : m_clients) {
                if (c->dead || c->attached != sid) continue;
                send_frame(c.get(), pid, proto::PANE_OUT, d, n);
                // Ack the sender's input after the output that reflects
                // it — the client's echo predictions compare against the
                // authoritative screen only once this arrives. The ack
                // carries the PTY's live termios ECHO bit: we own the
                // master, so password prompts (ECHO off) are a fact we
                // read, not a heuristic. An ECHO flip is also announced
                // unprompted (same frame, unchanged seq) so the client
                // suppresses predictions before the first password key.
                auto pit = m_panes.find(pid);
                if (pit != m_panes.end()) {
                    // "Echo off" only counts in canonical mode: that is
                    // the password-read shape (sudo, read -s). Readline
                    // prompts run raw with ECHO off and echo manually —
                    // they must stay predictable.
                    struct termios tio;
                    bool eoff = tcgetattr(pane->pty().fd(), &tio) == 0 &&
                                !(tio.c_lflag & ECHO) && (tio.c_lflag & ICANON);
                    bool flip = eoff != pit->second.echo_off;
                    pit->second.echo_off = eoff;
                    if (pit->second.in_client == c.get() &&
                        (flip || pit->second.in_seq != pit->second.acked_seq)) {
                        pit->second.acked_seq = pit->second.in_seq;
                        uint8_t buf[5];
                        memcpy(buf, &pit->second.acked_seq, 4);
                        buf[4] = eoff ? 1 : 0;
                        send_frame(c.get(), pid, proto::PANE_ACK, buf, 5);
                    }
                }
                // Backpressure: a QUIC peer slower than the PTY pauses
                // the producer; the shell blocks on the full PTY buffer.
                // (Shared-pane caveat: one slow client stalls the pane
                // for all — bounded-buffer resync is the step-5 answer.)
                if (c->quic && c->quic->queued() > net::QuicEngine::SEND_HIGH_WATER)
                    pane->set_read_paused(true);
            }
        };
        pane->on_dead = [this, pid](Pane *) {
            if (m_panes.erase(pid))
                m_dead_panes.push_back(pid);
        };
        ScreenBuffer &sb = pane->screen();
        sb.on_write_back = [pane](const std::string &d) { pane->write(d); };
        sb.on_title_change = [this, sid, pid](const std::string &title) {
            broadcast_event(sid, MsgType::TitleChanged, pid, title);
        };
        sb.on_cwd_change = [this, sid, pid, pane](const std::string &uri) {
            pane->cwd = uri;
            broadcast_event(sid, MsgType::CwdChanged, pid, uri);
        };
        sb.on_bell = [this, sid, pid]() {
            broadcast_event(sid, MsgType::Bell, pid, {});
        };
        // OSC 52 clipboard: needs most-recently-active routing (design);
        // not wired in v1.
    }

    // Recompute a window's cell-unit layout (this resizes panes and
    // their PTYs to the computed rects) and broadcast it.
    void relayout(Session &s, SrvWindow &win) {
        win.layout.compute_layout(0, 0, s.cols, s.rows, /*divider=*/1,
                                  /*cell_w=*/1, /*cell_h=*/1);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id) send_layout_to(c.get(), s, win);
    }

    void send_layout_to(Client *c, Session &s, SrvWindow &win) {
        proto::Writer w;
        w.u32(s.id);
        w.u32(win.id);
        w.u16((uint16_t)s.cols);
        w.u16((uint16_t)s.rows);
        w.u32((uint32_t)win.panes.size());
        for (auto &[pid, pane] : win.panes) {
            w.u32(pid);
            w.u16((uint16_t)pane->rect.x);
            w.u16((uint16_t)pane->rect.y);
            w.u16((uint16_t)pane->rect.w);
            w.u16((uint16_t)pane->rect.h);
        }
        send_control(c, MsgType::LayoutUpdate, w);
    }

    static std::string osc7_path(const std::string &uri) {
        const std::string prefix = "file://";
        if (uri.rfind(prefix, 0) != 0) return uri;
        auto slash = uri.find('/', prefix.size());
        return slash == std::string::npos ? "" : uri.substr(slash);
    }

    void split_pane(Session &s, uint32_t wid, Pane *target, SplitDir dir) {
        auto wit = std::find_if(s.windows.begin(), s.windows.end(),
                                [wid](const SrvWindow &w) { return w.id == wid; });
        if (wit == s.windows.end()) return;
        uint16_t pid = m_next_pane++;
        auto pane = std::make_unique<Pane>(target->screen().cols(),
                                           target->screen().rows(), m_config);
        if (!pane->spawn_shell(m_loop, osc7_path(target->cwd), s.agent_path)) return;
        if (!wit->layout.split(target, pane.get(), dir)) {
            pane->detach(m_loop);
            pane->pty().close();
            return;
        }
        wire_pane(s.id, pid, pane.get());
        wit->panes.emplace_back(pid, std::move(pane));
        m_panes[pid] = {s.id, wid, wit->panes.back().second.get()};
        relayout(s, *wit);
    }

    void resume_session_panes(uint32_t sid) {
        auto it = m_sessions.find(sid);
        if (it == m_sessions.end()) return;
        // Only resume if no other attached client is still above water.
        for (auto &c : m_clients)
            if (!c->dead && c->attached == sid && c->quic &&
                c->quic->queued() > net::QuicEngine::SEND_LOW_WATER)
                return;
        for (auto &win : it->second.windows)
            for (auto &[pid, pane] : win.panes)
                pane->set_read_paused(false);
    }

    void broadcast_event(uint32_t sid, MsgType t, uint16_t pid, const std::string &str_arg) {
        proto::Writer w;
        w.u32(pid);
        if (t == MsgType::TitleChanged || t == MsgType::CwdChanged) w.str(str_arg);
        for (auto &c : m_clients)
            if (!c->dead && c->hello && c->attached == sid)
                send_control(c.get(), t, w);
    }

    void close_session(Session &s, const char *reason) {
        rivt::logmsg("rivtd: closing session %u (%s)\n", s.id, reason);
        if (s.agent_fd >= 0) {
            m_loop.remove_fd(s.agent_fd);
            ::close(s.agent_fd);
            unlink(s.agent_path.c_str());
            s.agent_fd = -1;
        }
        for (auto it = m_agent_streams.begin(); it != m_agent_streams.end();) {
            if (it->second.sid == s.id) {
                m_loop.remove_fd(it->second.fd);
                ::close(it->second.fd);
                it = m_agent_streams.erase(it);
            } else {
                ++it;
            }
        }
        for (auto &win : s.windows)
            for (auto &[pid, pane] : win.panes) {
                m_panes.erase(pid);
                pane->detach(m_loop);
                pane->pty().close();  // HUPs the child
            }
        proto::Writer w;
        w.u32(s.id);
        for (auto &c : m_clients)
            if (!c->dead && c->attached == s.id) {
                send_control(c.get(), MsgType::SessionClosed, w);
                c->attached = 0;
            }
    }

    // ---------------- SSH agent forwarding ----------------

    // Listen on a per-session unix socket; shells of the session get it
    // as SSH_AUTH_SOCK. Path is deterministic (survives daemon upgrade).
    void session_agent_init(Session &s) {
        std::string dir = m_path.substr(0, m_path.rfind('/'));
        s.agent_path = dir + "/agent-" + std::to_string(s.id) + ".sock";
        unlink(s.agent_path.c_str());
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (s.agent_path.size() >= sizeof(addr.sun_path)) return;
        strcpy(addr.sun_path, s.agent_path.c_str());
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 8) < 0) {
            ::close(fd);
            return;
        }
        chmod(s.agent_path.c_str(), 0600);
        s.agent_fd = fd;
        uint32_t sid = s.id;
        m_loop.add_fd(fd, [this, sid](uint32_t) { agent_accept(sid); });
    }

    void agent_accept(uint32_t sid) {
        auto sit = m_sessions.find(sid);
        if (sit == m_sessions.end()) return;
        int cfd;
        while ((cfd = accept4(sit->second.agent_fd, nullptr, nullptr,
                              SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
            // Bridge to the most recently connected client attached to
            // this session; with none, refuse — ssh falls back cleanly.
            Client *target = nullptr;
            for (auto it = m_clients.rbegin(); it != m_clients.rend(); ++it)
                if (!(*it)->dead && (*it)->attached == sid) { target = it->get(); break; }
            if (!target) {
                ::close(cfd);
                continue;
            }
            uint32_t id = m_next_agent_stream++;
            m_agent_streams[id] = {cfd, sid, target, {}, 0, false};
            m_loop.add_fd(cfd, [this, id](uint32_t ev) { agent_stream_event(id, ev); });
            proto::Writer w;
            w.u32(id);
            send_control(target, MsgType::AgentOpen, w);
            dbg("rivtd: agent stream %u opened (session %u)", id, sid);
        }
    }

    void agent_stream_event(uint32_t id, uint32_t ev) {
        auto it = m_agent_streams.find(id);
        if (it == m_agent_streams.end()) return;
        AgentStream &st = it->second;
        if (ev & EV_WRITE) agent_stream_flush(st);
        if (ev & EV_READ) {
            uint8_t buf[4096];
            for (;;) {
                ssize_t n = recv(st.fd, buf, sizeof buf, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    close_agent_stream(id, true);
                    return;
                }
                if (n == 0) {
                    close_agent_stream(id, true);
                    return;
                }
                if (st.client && !st.client->dead) {
                    proto::Writer w;
                    w.u32(id);
                    w.bytes(buf, (size_t)n);
                    send_control(st.client, MsgType::AgentData, w);
                }
            }
        }
        if (ev & (EV_HUP | EV_ERR)) close_agent_stream(id, true);
    }

    void agent_stream_flush(AgentStream &st) {
        while (st.out_off < st.out.size()) {
            ssize_t n = send(st.fd, st.out.data() + st.out_off,
                             st.out.size() - st.out_off, MSG_NOSIGNAL);
            if (n > 0) { st.out_off += (size_t)n; continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0 && errno == EINTR) continue;
            break;  // hard error: the read side will see HUP and clean up
        }
        if (st.out_off == st.out.size()) {
            st.out.clear();
            st.out_off = 0;
        }
        bool want = st.out_off < st.out.size();
        if (want != st.write_armed) {
            st.write_armed = want;
            m_loop.modify_fd(st.fd, EV_READ | (want ? EV_WRITE : 0));
        }
    }

    void close_agent_stream(uint32_t id, bool notify) {
        auto it = m_agent_streams.find(id);
        if (it == m_agent_streams.end()) return;
        AgentStream st = it->second;
        m_agent_streams.erase(it);
        m_loop.remove_fd(st.fd);
        ::close(st.fd);
        if (notify && st.client && !st.client->dead) {
            proto::Writer w;
            w.u32(id);
            send_control(st.client, MsgType::AgentClose, w);
        }
        dbg("rivtd: agent stream %u closed", id);
    }

    // ---------------- deferred cleanup ----------------

    // Remove a pane whose PTY is already dead/closed: notify clients,
    // collapse the layout, close the window when it was the window's
    // last pane, and end the session when it was the last window.
    void remove_dead_pane(uint16_t pid) {
        for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
            Session &s = it->second;
            for (auto wit = s.windows.begin(); wit != s.windows.end(); ++wit) {
                auto pit = std::find_if(wit->panes.begin(), wit->panes.end(),
                                        [pid](auto &p) { return p.first == pid; });
                if (pit == wit->panes.end()) continue;
                // Reap here if we beat the signalfd handler to it, so the
                // exit status always lands next to the pane-exit line.
                char how[64] = "not yet reaped";
                pid_t cpid = pit->second->pty().child_pid();
                int status = 0;
                if (cpid > 0 && waitpid(cpid, &status, WNOHANG) == cpid) {
                    if (WIFEXITED(status))
                        snprintf(how, sizeof how, "exit status %d", WEXITSTATUS(status));
                    else if (WIFSIGNALED(status))
                        snprintf(how, sizeof how, "killed by signal %d", WTERMSIG(status));
                }
                rivt::logmsg("rivtd: pane %u exited (%s; session %u window %u, "
                             "%zu pane(s) remain in window)\n",
                             pid, how, s.id, wit->id, wit->panes.size() - 1);

                proto::Writer w;
                w.u32(pid);
                for (auto &c : m_clients)
                    if (!c->dead && c->attached == s.id)
                        send_control(c.get(), MsgType::PaneExited, w);

                wit->layout.remove(pit->second.get());
                pit->second->detach(m_loop);
                wit->panes.erase(pit);
                if (!wit->panes.empty()) {
                    relayout(s, *wit);
                    return;
                }
                close_window(s, wit->id);
                return;
            }
        }
    }

    void sweep() {
        for (uint16_t pid : m_dead_panes) remove_dead_pane(pid);
        m_dead_panes.clear();

        if (m_sweep_clients) {
            m_sweep_clients = false;
            for (auto &c : m_clients)
                if (c->dead) {
                    for (auto &[pid, ref] : m_panes)
                        if (ref.in_client == c.get()) ref.in_client = nullptr;
                    for (auto it = m_agent_streams.begin(); it != m_agent_streams.end();) {
                        if (it->second.client == c.get()) {
                            m_loop.remove_fd(it->second.fd);
                            ::close(it->second.fd);
                            it = m_agent_streams.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    if (c->quic) {
                        c->quic->user = nullptr;
                        m_quic->close_conn(c->quic);
                    } else {
                        m_loop.remove_fd(c->fd);
                        close(c->fd);
                    }
                }
            std::erase_if(m_clients, [](auto &c) { return c->dead; });
        }
    }

    // ---------------- upgrade / handover ----------------

    static void encode_layout_node(proto::Writer &w, const LayoutNode *n,
                                   const std::vector<std::pair<uint16_t, std::unique_ptr<Pane>>> &panes) {
        if (n->is_leaf()) {
            uint16_t pid = 0;
            for (auto &[id, p] : panes)
                if (p.get() == n->pane) pid = id;
            w.u8(0);
            w.u16(pid);
        } else {
            w.u8(1);
            w.u8(n->split_dir == SplitDir::Vertical ? 0 : 1);
            w.u32((uint32_t)(n->ratio * 1000000.0f));
            encode_layout_node(w, n->first.get(), panes);
            encode_layout_node(w, n->second.get(), panes);
        }
    }

    static std::unique_ptr<LayoutNode> decode_layout_node(
        proto::Reader &r, const std::unordered_map<uint16_t, Pane *> &by_id) {
        auto n = std::make_unique<LayoutNode>();
        uint8_t type = r.u8();
        if (!r.ok) return nullptr;
        if (type == 0) {
            auto it = by_id.find(r.u16());
            if (it == by_id.end()) return nullptr;
            n->pane = it->second;
        } else {
            n->split_dir = r.u8() == 0 ? SplitDir::Vertical : SplitDir::Horizontal;
            n->ratio = (float)r.u32() / 1000000.0f;
            n->first = decode_layout_node(r, by_id);
            n->second = decode_layout_node(r, by_id);
            if (!n->first || !n->second) return nullptr;
        }
        return n;
    }

    void upgrade() {
        if (m_exe.empty()) {
            rivt::logmsg("rivtd: upgrade: /proc/self/exe unknown\n");
            return;
        }
        proto::Writer w;
        w.u32(0x444E4852);  // "RHND"
        w.u32(HANDOVER_VERSION);
        w.u32(m_next_sid);
        w.u32(m_next_wid);
        w.u16(m_next_pane);
        w.u32((uint32_t)m_sessions.size());
        for (auto &[sid, sess] : m_sessions) {
            w.u32(sid);
            w.str(sess.name);
            w.u16((uint16_t)sess.cols);
            w.u16((uint16_t)sess.rows);
            w.u32((uint32_t)sess.windows.size());
            for (auto &win : sess.windows) {
                w.u32(win.id);
                w.u32((uint32_t)win.panes.size());
                for (auto &[pid, pane] : win.panes) {
                    w.u16(pid);
                    w.i32(pane->pty().fd());
                    w.i32((int32_t)pane->pty().child_pid());
                    w.str(pane->cwd);
                    auto blob = proto::Snapshot::serialize(pane->screen(), pane->parser(), -1);
                    w.u32((uint32_t)blob.size());
                    w.bytes(blob.data(), blob.size());
                    // The master fd must survive the exec.
                    fcntl(pane->pty().fd(), F_SETFD, 0);
                }
                encode_layout_node(w, win.layout.root(), win.panes);
            }
        }

        std::string file = m_path + ".handover";
        int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || ::write(fd, w.buf.data(), w.buf.size()) != (ssize_t)w.buf.size()) {
            rivt::logmsg("rivtd: upgrade: cannot write %s\n", file.c_str());
            if (fd >= 0) close(fd);
            return;
        }
        close(fd);

        // QUIC sockets die silently across exec; close connections
        // properly so clients reconnect immediately instead of waiting
        // out the idle timeout. (Unix clients see EOF on their own.)
        if (m_quic)
            for (auto &c : m_clients)
                if (c->quic) m_quic->close_conn(c->quic);

        rivt::logmsg("rivtd: upgrading via exec (%zu sessions)\n", m_sessions.size());
        std::string port = std::to_string(m_listen_port);
        if (m_listen_port > 0)
            execl(m_exe.c_str(), "rivtd", "--socket", m_path.c_str(),
                  "--handover", file.c_str(), "--listen", port.c_str(),
                  "--name", m_name.c_str(), (char *)nullptr);
        else
            execl(m_exe.c_str(), "rivtd", "--socket", m_path.c_str(),
                  "--handover", file.c_str(), (char *)nullptr);
        // exec failed: nothing was torn down (fds/CLOEXEC aside), keep serving.
        perror("rivtd: upgrade: exec");
        unlink(file.c_str());
    }

    void restore_handover(const std::string &file) {
        std::vector<uint8_t> data;
        int fd = open(file.c_str(), O_RDONLY);
        if (fd < 0) return;
        char buf[65536];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) data.insert(data.end(), buf, buf + n);
        close(fd);
        unlink(file.c_str());

        proto::Reader r(data.data(), data.size());
        if (r.u32() != 0x444E4852 || r.u32() != HANDOVER_VERSION || !r.ok) {
            rivt::logmsg("rivtd: incompatible handover file, starting fresh\n");
            return;
        }
        m_next_sid = r.u32();
        m_next_wid = r.u32();
        m_next_pane = r.u16();
        uint32_t nsess = r.u32();
        for (uint32_t i = 0; i < nsess && r.ok; i++) {
            Session sess;
            sess.id = r.u32();
            sess.name = r.str();
            sess.cols = r.u16();
            sess.rows = r.u16();
            uint32_t nwin = r.u32();
            for (uint32_t j = 0; j < nwin && r.ok; j++) {
                SrvWindow win;
                win.id = r.u32();
                uint32_t npanes = r.u32();
                std::unordered_map<uint16_t, Pane *> by_id;
                for (uint32_t k = 0; k < npanes && r.ok; k++) {
                    uint16_t pid = r.u16();
                    int pfd = r.i32();
                    pid_t child = (pid_t)r.i32();
                    std::string cwd = r.str();
                    uint32_t bl = r.u32();
                    if (!r.ok || r.remaining() < bl) { r.ok = false; break; }

                    auto pane = std::make_unique<Pane>(sess.cols, sess.rows, m_config);
                    if (!proto::Snapshot::deserialize(pane->screen(), pane->parser(),
                                                      r.p, bl)) {
                        rivt::logmsg("rivtd: handover: bad snapshot for pane %u\n", pid);
                        close(pfd);
                        r.skip(bl);
                        continue;
                    }
                    r.skip(bl);
                    fcntl(pfd, F_SETFD, FD_CLOEXEC);  // re-arm for the next exec
                    pane->adopt_shell(m_loop, pfd, child);
                    pane->cwd = cwd;
                    wire_pane(sess.id, pid, pane.get());
                    win.panes.emplace_back(pid, std::move(pane));
                    by_id[pid] = win.panes.back().second.get();
                    m_panes[pid] = {sess.id, win.id, win.panes.back().second.get()};
                }
                auto root = decode_layout_node(r, by_id);
                if (root) win.layout.set_root(std::move(root));
                else if (!win.panes.empty()) win.layout.init(win.panes[0].second.get());
                if (!win.panes.empty()) {
                    // Rects are not part of the handover; recompute them
                    // (this also re-syncs pane/PTY sizes to the layout).
                    win.layout.compute_layout(0, 0, sess.cols, sess.rows, 1, 1, 1);
                    sess.windows.push_back(std::move(win));
                }
            }
            if (!sess.windows.empty()) m_sessions.emplace(sess.id, std::move(sess));
        }
        rivt::logmsg("rivtd: handover restored %zu session(s)\n", m_sessions.size());
    }

    std::string m_path;
    std::string m_handover_in;
    std::string m_exe;
    int m_listen_port = 0;
    std::string m_name;
    Config m_config;
    EventLoop m_loop;
    // Declared after m_loop so they're destroyed before it: each
    // unregisters its fd/timer from the loop in its destructor.
    std::unique_ptr<net::Identity> m_identity;
    std::unique_ptr<net::QuicEngine> m_quic;
    std::unique_ptr<net::Signaling> m_signaling;
    // Relays rotate: a Cloudflare TURN allocation silently forwards
    // only ~10 distinct peer flows over its lifetime (measured; flow 11+
    // is dropped with the control plane green, and the budget never
    // recovers). Each answered offer costs the client one flow, so a
    // fresh allocation is made every few answers; old relays keep
    // serving their established sessions until they have been silent
    // long enough to reap.
    std::unordered_map<uint32_t, AgentStream> m_agent_streams;
    uint32_t m_next_agent_stream = 1;
    const uint32_t m_epoch = (uint32_t)time(nullptr) ^ (uint32_t)getpid();

    std::vector<std::unique_ptr<net::TurnRelay>> m_relays;
    net::TurnRelay *m_turn = nullptr;  // current: advertised in answers
    int m_turn_answers = 0;            // answers served by m_turn
    int m_turn_idle_ticks = 0;         // minutes with no QUIC clients
    std::string m_cred_user, m_cred_pass, m_cred_host;
    uint16_t m_cred_port = 0;
    time_t m_cred_fetched = 0;
    struct sockaddr_in m_reflexive {};  // our QUIC socket's public v4 addr
    int m_listen_fd = -1;
    int m_sig_fd = -1;
    std::vector<std::unique_ptr<Client>> m_clients;
    std::map<uint32_t, Session> m_sessions;
    std::unordered_map<uint16_t, PaneRef> m_panes;
    uint32_t m_next_sid = 1;
    uint32_t m_next_wid = 1;
    uint16_t m_next_pane = 1;
    std::vector<uint16_t> m_dead_panes;
    bool m_sweep_clients = false;
};

} // namespace rivt

static std::string default_socket_path() {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    std::string dir = rt ? std::string(rt) + "/rivt"
                         : "/tmp/rivt-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700);
    return dir + "/daemon.sock";
}

// --- interactive first-run setup -------------------------------------

static std::string prompt(const char *msg) {
    fputs(msg, stdout);
    fflush(stdout);
    char line[1024];
    if (!fgets(line, sizeof line, stdin)) return {};
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Write a systemd --user unit for this exact binary and enable it.
static bool install_systemd_unit() {
    // Sessions that bypass pam_systemd (containers, `ssh host cmd`,
    // sudo -u shells) lack XDG_RUNTIME_DIR even when a systemd --user
    // instance is running; systemctl --user then fails with ENOMEDIUM.
    if (!getenv("XDG_RUNTIME_DIR")) {
        std::string rt = "/run/user/" + std::to_string(getuid());
        struct stat st;
        if (stat((rt + "/systemd").c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            setenv("XDG_RUNTIME_DIR", rt.c_str(), 1);
    }

    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) { rivt::logmsg("cannot resolve own path\n"); return false; }
    exe[n] = 0;

    const char *cfg = getenv("XDG_CONFIG_HOME");
    std::string dir = cfg && *cfg ? std::string(cfg) + "/systemd/user"
                                  : std::string(getenv("HOME") ? getenv("HOME") : ".") +
                                        "/.config/systemd/user";
    std::string acc;
    for (size_t i = 0; i <= dir.size(); i++) {
        if ((i == dir.size() || dir[i] == '/') && !acc.empty()) mkdir(acc.c_str(), 0755);
        if (i < dir.size()) acc += dir[i];
    }
    std::string path = dir + "/rivtd.service";
    FILE *f = fopen(path.c_str(), "w");
    if (!f) { rivt::logmsg("cannot write %s\n", path.c_str()); return false; }
    fprintf(f,
            "[Unit]\nDescription=rivt terminal session daemon\n"
            "After=network-online.target\nWants=network-online.target\n\n"
            "[Service]\nExecStart=%s --listen\nRestart=on-failure\nRestartSec=2\n\n"
            "[Install]\nWantedBy=default.target\n",
            exe);
    fclose(f);

    if (system("systemctl --user daemon-reload") != 0 ||
        system("systemctl --user enable --now rivtd") != 0) {
        rivt::logmsg(                "\nInstalled %s but could not enable it (no systemd --user session?).\n"
                "Enable manually: systemctl --user enable --now rivtd\n",
                path.c_str());
        return false;
    }
    // Best-effort: keep it running across logout / at boot.
    if (system("loginctl enable-linger \"$USER\" >/dev/null 2>&1") != 0)
        rivt::logmsg("note: run `loginctl enable-linger` to start at boot without login\n");
    return true;
}

static int run_setup(int argc, char **argv) {
    std::string code;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-' && strcmp(argv[i], "setup") != 0) { code = argv[i]; break; }

    printf("rivt server setup\n\n");
    if (!rivt::net::interactive_enroll(code)) return 1;

    std::string ans = prompt("\nRun rivtd as a background service (systemd --user)? [Y/n] ");
    if (ans.empty() || ans[0] == 'y' || ans[0] == 'Y') {
        if (install_systemd_unit())
            printf("\nrivtd is running and will start on boot.\n");
    } else {
        printf("\nStart it yourself with: rivtd --listen\n");
    }
    char h[256] = "this-host";
    gethostname(h, sizeof h - 1);
    std::string host = h;
    auto d = host.find('.');
    if (d != std::string::npos) host.resize(d);
    printf("\nDone. Reach this box from another device with:  rivt --connect %s\n",
           host.c_str());
    return 0;
}

// Ask a running daemon to upgrade itself (works across protocol versions).
static int request_upgrade(const std::string &path) {
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return 1;
    strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        rivt::logmsg("rivtd: no daemon at %s\n", path.c_str());
        return 1;
    }
    uint8_t hdr[rivt::proto::FRAME_HEADER_SIZE];
    rivt::proto::encode_frame_header(hdr, {0, 0, (uint16_t)rivt::proto::MsgType::UpgradeDaemon});
    (void)!write(fd, hdr, sizeof hdr);
    close(fd);
    printf("rivtd: upgrade requested\n");
    return 0;
}

int main(int argc, char **argv) {
    std::string path, handover, name;
    bool upgrade = false;
    int listen_port = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--handover") && i + 1 < argc) handover = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--upgrade")) upgrade = true;
        else if (!strcmp(argv[i], "--listen")) {
            listen_port = 7433;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) listen_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--fingerprint")) {
            auto id = rivt::net::Identity::load_or_create();
            if (!id) return 1;
            printf("fingerprint: %s\ncert: %s\nauthorized peers: %s\n",
                   id->fingerprint().c_str(), id->cert_path().c_str(),
                   rivt::net::Identity::authorized_bundle_path().c_str());
            return 0;
        }
    }
    // Interactive first-run setup.
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "setup")) return run_setup(argc, argv);

    // Install/enable the systemd --user service for an already-
    // configured daemon (setup offers the same thing after enrollment).
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "install")) {
            if (!install_systemd_unit()) return 1;
            printf("rivtd is running under systemd --user and will start on boot.\n");
            return 0;
        }

    // Pairing / set verbs (foreground, no daemon).
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "pair") || !strcmp(argv[i], "join") ||
            !strcmp(argv[i], "init")) {
            auto id = rivt::net::Identity::load_or_create();
            if (!id) return 1;
            if (!strcmp(argv[i], "pair")) return rivt::net::pair_invite(*id) ? 0 : 1;
            if (!strcmp(argv[i], "init"))
                return rivt::net::sync_membership(*id, true).empty() ? 1 : 0;
            if (i + 1 >= argc) { rivt::logmsg("usage: rivtd join <code>\n"); return 1; }
            return rivt::net::pair_join(argv[i + 1], *id) ? 0 : 1;
        }
    }

    if (path.empty()) path = default_socket_path();
    if (upgrade) return request_upgrade(path);
    if (name.empty()) {
        char host[256] = "rivt";
        gethostname(host, sizeof host - 1);
        name = host;
        auto dot = name.find('.');
        if (dot != std::string::npos) name.resize(dot);
    }

    // Daemon mode from here on: copy every log line to syslog. stderr
    // stays active too (it points at the .log file when spawned detached).
    rivt::log_to_syslog("rivtd");
    rivt::Daemon d(path, handover, listen_port, name);
    if (!d.init()) return 1;
    rivt::logmsg("rivtd: listening on %s\n", path.c_str());
    return d.run();
}
