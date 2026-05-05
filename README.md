# 0pty

Run your agents on a server instead of a local terminal. Reconnect to your session after closing the terminal or restarting your computer. Connect multiple terminals or your phone to the same session.

0pty is a dependency-free Linux PTY persistence daemon/client pair for long-running interactive CLI programs: coding agents, shells, REPLs, debuggers, and anything else that expects a PTY. The server keeps a PTY alive, streams raw output, and replays recent output from a ring buffer when a client reconnects. The client is a byte pipe: stdin to the socket, socket to stdout.

---

## The Problem

Interactive CLI agents save conversations as JSON, but if the terminal crashes hard enough, the conversation does not always get written cleanly. `/resume` usually works. Sometimes it does not, and you lose hours of context, design decisions, and reasoning.

The fix: Run it inside a daemon on your dev machine that holds the PTY open permanently. Connect to it from whatever terminal you want. If the terminal crashes, the process doesn't. Reconnect, pick up where you left off. The conversation is never at risk because the process never died.

This isn't a terminal emulator. It's a PTY babysitter with a ring buffer and a TCP socket.

Side benefit: you can go to lunch and connect via termux on your phone and keep coding!

---

## The Name

0pt is the font size that renders nothing. That's the client: zero rendering, zero parsing. Pronounced "op-tee."

---

## Build

```sh
make
```

This builds:

- `bin/0pty`
- `bin/0pty-server`

The build uses `cc` by default, or `gcc`/another C11 compiler if you set `CC`. The server links `pthread` and `util`; the client links `pthread`.

`make test` builds both binaries and runs the protocol/ring-buffer regression test.

## Recommended Workflow

Start the agent directly, not a shell:

```sh
0pty claude01 start claude --resume
```

This means stop, restart, and crash recovery workflows can know what
command to run, what directory to use, and how to ask the agent to exit cleanly.

Avoid this for agent sessions:

```sh
0pty claude01 start bash
# then: claude --resume
```

That works for a persistent shell, but stop and restart become ambiguous because
0pty cannot know what is running inside the shell.

## Named Sessions

The easiest workflow is named sessions. `start` allocates a localhost port,
writes a session record under `~/.0pty/sessions`, launches `0pty-server`, and
attaches immediately.

```sh
# start persistent agent sessions in the current directory and attach
0pty claude01 start claude
0pty codexCoder start codex

# reattach later
0pty connect claude01
0pty connect codexCoder

# if there is exactly one alive session, this connects to it
0pty connect

# shorthand reattach
0pty claude01
0pty codexCoder

# list user sessions
0pty list

# restart a dead session from its stored cwd and argv
0pty restart codexCoder

# ask a live session to exit cleanly
0pty stop codexCoder
```

Commands are passed as normal argv, so extra flags work:

```sh
0pty copilot-sonnet start copilot --yolo
0pty connect copilot-sonnet
```

The conventional order also works:

```sh
0pty start codex01 -- codex
```

Session names can contain letters, digits, dots, dashes, and underscores.
Session listing is user-scoped: `0pty list` reads only `~/.0pty/sessions` for
the current user and checks liveness with a short TCP connect probe.
`0pty restart NAME` works only for dead sessions. It reuses the start-time
working directory and exact argv stored in the session file; it refuses to
replace an alive session.
`0pty stop NAME` sends the session's stored `graceful_input` to the live PTY
and waits for the server to shut down. The default graceful input is `/exit\n`.

## Raw Endpoint Mode

Default endpoints are `127.0.0.1:6077`.

Endpoint forms accepted by the shared parser:

- `host:port`
- `:port` for localhost on a different port
- `[ipv6][:port]`
- `host` for the default port

Typical workflow:

```sh
# on the dev box
bin/0pty-server -b 127.0.0.1:6077

# from another terminal or machine on the same network
bin/0pty 127.0.0.1:6077
```

To run a specific persistent command:

```sh
bin/0pty-server -b 127.0.0.1:6077 -- claude --resume
```

The server is meant to stay bound to localhost or a Tailscale interface. Do not expose it on `0.0.0.0`.

## Protocol

Frames are length-prefixed. Each message is a 4-byte big-endian length, a 1-byte message type, and the payload.

The current message set covers:

- `HELLO` / `RECONNECT`
- `WELCOME`
- `STDIN` / `STDOUT`
- `RESIZE`
- `ACK`
- `REPLAY`
- `ERROR`
- `CONTROL_SHUTDOWN`

Sequence numbers identify the server byte stream. The client remembers the last sequence it saw, sends that on reconnect, and the server replies with a replay frame followed by live output.

## Security

The transport is not SSH. Bind only to `127.0.0.1` or a private/Tailscale address and keep the optional shared token enabled for non-local use. The service file under `systemd/` defaults to localhost for that reason.

Named session files are user-scoped and stored under `~/.0pty/sessions` with
0600 permissions. The `control_token` in that file is the authority for
`0pty stop`; anyone who can read the session file can stop that user's session.

## v0.1.0

v0.1.0 includes the core persistent PTY server/client, named sessions, `list`,
smart `connect`, and manual `restart` for dead sessions.

## v0.2.0

v0.2.0 adds graceful named-session shutdown with `0pty stop NAME`, per-session
control tokens, TCP_NODELAY for lower interactive latency, atomic session file
writes, and user-scoped log paths under `~/.0pty/logs`.

Automatic supervision and disk-persisted server scrollback are intentionally
left for later releases.

## Reconnect Workflow

1. The server keeps the PTY and ring buffer alive even when no clients are attached.
2. The client disconnects and later reconnects with its last known sequence number.
3. The server replays buffered output starting at that sequence.
4. Live PTY output resumes immediately after replay.

That is the whole design: persistent process, raw byte stream, bounded replay buffer.
