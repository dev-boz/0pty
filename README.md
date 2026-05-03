# 0pty — Persistent PTY Daemon

## The Problem

Claude Code saves conversations as JSON, but if the terminal crashes hard enough, the conversation doesn't get written or gets corrupted. `/resume` usually works. Sometimes it doesn't, and you lose hours of context, design decisions, and reasoning.

The fix: don't run Claude Code inside a terminal that can crash. Run it inside a daemon on the dev box that holds the PTY open permanently. Connect to it from whatever terminal you want. If the terminal crashes, the process doesn't. Reconnect, pick up where you left off. The conversation is never at risk because the process never died.

This isn't a terminal emulator. It's a PTY babysitter with a ring buffer and a TCP socket.

---

## The Name

0pt is the font size that renders nothing. That's the client — zero rendering, zero parsing. It pipes bytes between your terminal and the server. Pronounced "op-tee."

---

## What This Is

**Server** (`0pty-server`) — a daemon on the Linux dev box. Holds a PTY open, spawns a shell or command. Accepts TCP connections. Pipes bytes between the PTY and connected clients. Keeps a ring buffer of recent PTY output for reconnect replay. Optionally saves scrollback to disk for disaster recovery.

**Client** (`0pty`) — a tiny program you run inside your existing terminal (Windows Terminal, Alacritty, whatever). Connects to the server over TCP/Tailscale. Pipes stdin to the server, pipes server output to stdout. On reconnect, receives ring buffer replay so the screen rebuilds.

The client is essentially:

```
stdin  → socket
socket → stdout
```

Your terminal does all the rendering. There is one VT parser (your terminal's), one state machine, nothing fights. The process lives on the server. The display lives on the client. They are connected by a byte pipe.

---

## Prior Art

- **dtach / abduco** — hold a PTY open, allow attach/detach. The closest existing tools. What they lack: ring buffer replay on reconnect (you reattach to a blank screen), TCP listening (they use Unix domain sockets), and network-aware buffering.

- **tmux / screen** — session persistence plus a full multiplexer and second terminal state machine. Solves the persistence problem but creates the double-state-machine problem (broken scroll, paste, mouse).

- **Mosh** — transport resilience and predictive echo over UDP. Solves latency and roaming but still expects a local terminal to re-parse escape codes.

- **Eternal Terminal** — reconnectable SSH. Solves reconnect but doesn't change the rendering architecture.

- **WezTerm mux** — the closest full product. Server-side VT parsing, remote attach, persistent sessions. But it's a massive general-purpose terminal. 0pty is dtach with a ring buffer.

---

## Architecture

### Server

A systemd user service on the dev box. Always running.

- Spawns a PTY with a configured command (default: `$SHELL`, but mainly `claude` or `claude --resume`)
- Listens on TCP (bound to Tailscale interface or localhost)
- On client connect: replays ring buffer so the screen rebuilds, then streams live PTY output
- Pipes client input to PTY stdin
- Pipes PTY stdout to all connected clients
- Handles PTY resize (SIGWINCH) when client sends resize messages
- Keeps PTY alive when no clients are connected
- Maintains a ring buffer of raw PTY output (configurable, default 1MB — enough to rebuild any reasonable screen state)
- Optionally writes ring buffer to disk periodically for server-crash recovery

### Client

A small program you run inside your terminal.

- Connects to `0pty-server` over TCP
- Sets local terminal to raw mode
- Pipes stdin → socket, socket → stdout
- Sends terminal resize events to server when window size changes
- On disconnect: exits cleanly (or auto-retries with a flag)

The client has no VT parser, no state machine, no grid, no rendering. Your terminal handles all of that.

### Scrollback Snapshots — Mirrored Redundancy

Two layers of protection:

**Client-side:** the client periodically saves received PTY output to a local file. If the client machine survives but the connection was lost, you have full scrollback history on disk. This is just appending bytes to a file — trivially cheap.

**Server-side:** the ring buffer itself can be persisted to disk periodically. If the server reboots, the ring buffer is gone from memory but the disk copy survives. A new client can't reconnect to the dead process, but it can retrieve the scrollback history to see exactly what was happening.

Between the two, you always have the conversation history. The process itself can't survive a server reboot (that would require CRIU-style process checkpointing, which is fragile and not worth the complexity), but for Claude Code, the JSON conversation file and git state are the real checkpoints. The scrollback snapshots are for the cases where those aren't enough.

---

## Protocol

Minimal. Barely a protocol.

**Handshake:**
```
Client → Server: Hello { version: u8, terminal_size: (cols, rows) }
Server → Client: Welcome { ring_buffer_bytes }
```

**Steady state:**
```
Client → Server: Data { bytes }          — keyboard input
Client → Server: Resize { cols, rows }   — terminal size changed
Server → Client: Data { bytes }          — PTY output, raw
```

**Reconnect:**
```
Client → Server: Reconnect { version: u8, last_seq: u64, terminal_size: (cols, rows) }
Server → Client: Replay { from_seq, bytes }
```

Sequence numbers on the server's byte stream. Client periodically ACKs its position. Server keeps the ring buffer from the oldest unACKed position. On reconnect, client sends its last known sequence number, server replays from there.

Framing: length-prefixed messages over TCP. Use `tokio` + `LengthDelimitedCodec` or even just a hand-rolled 4-byte length prefix. The messages are so simple it barely matters.

Auth: bind to Tailscale interface or localhost. Optional shared token in the handshake for non-Tailscale setups. Don't bind to 0.0.0.0.

---

## Resize

Client detects terminal size change (SIGWINCH on the local side). Sends `Resize { cols, rows }` to server. Server calls `ioctl(TIOCSWINSZ)` on the PTY. The running process (Claude Code, shell, vim, whatever) receives SIGWINCH and redraws.

No freezing, no ACK, no complexity. The resize goes straight to the PTY and the process handles it the same way it would if you resized a local terminal. The byte stream carries the redraw output back to the client naturally.

---

## Multi-Client

Multiple clients can connect simultaneously. All receive the same PTY output. All can send input (last writer wins, same as two people typing on the same keyboard). Resize comes from whichever client resized last.

This is useful for pair debugging — someone else can watch your Claude Code session — but the primary use case is one client at a time.

---

## What 0pty Is Not

- **Not a terminal emulator** — your existing terminal does all the rendering
- **Not a multiplexer** — no tmux, no panes, no window management
- **Not a remote desktop** — it's a byte pipe
- **Not an SSH replacement** — it's a PTY persistence daemon you connect to over an already-secured network

---

## Build Sequence

### Phase 0 — The whole thing, basically

Server: tokio TCP listener, PTY via `portable-pty` or raw `openpty`, ring buffer, byte pipe. Client: raw mode terminal, TCP connect, stdin/stdout pipe, resize forwarding.

This is a small enough project that Phase 0 is almost the entire product. The server is probably 300 lines of Rust. The client is probably 100.

**Success = Claude Code running on the dev box, visible in Windows Terminal via 0pty, you close Windows Terminal, reopen, reconnect, Claude Code is still running and the screen rebuilds from the ring buffer.**

### Phase 1 — Robustness

Sequence-numbered reconnect. Disk-persisted ring buffer. Client-side scrollback logging. Auto-reconnect. systemd service file.

### Phase 2 — Convenience

Named sessions (run multiple 0pty-server instances). A config file for default commands, ring buffer size, bind address. A `0pty list` command to see running sessions. Tab completion.

---

## End State

Run `0pty connect devbox:session1` inside Windows Terminal. Claude Code is running. Close the laptop. Open it tomorrow. Run the command again. Still there. Scroll up. Full history. Nothing was lost.

It's dtach with a ring buffer, over TCP, for the network age.
