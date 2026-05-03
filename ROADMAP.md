# 0pty Roadmap

## v0.1.0 Current

- Persistent PTY server/client.
- Raw byte-pipe client with no terminal emulation.
- Ring-buffer replay on reconnect.
- Named sessions stored under `~/.0pty/sessions`.
- `0pty list` with live/dead status.
- Smart `0pty connect` session selection.
- `0pty restart NAME` for dead sessions using stored cwd and argv.

## v0.2.0 Stop

Add graceful session shutdown.

Planned behavior:

- `0pty stop NAME`
- Session files get a `control_token`.
- Server accepts a control shutdown frame on the same TCP listener.
- Server validates the token before acting.
- Default graceful path writes `graceful_input` to the PTY, currently `/exit\n`.
- If the child does not exit after a grace period, send `SIGHUP`.
- If it still does not exit, send `SIGTERM`.
- Keep `SIGKILL` for a later explicit `--force` path.

Design notes:

- The control token is authority. PID is optional metadata, not authority.
- A PID can be stale or recycled; a server that knows the token is the session.
- The token lives in the user-scoped session file, so anyone who can read
  `~/.0pty/sessions` can control that user's sessions. This matches the current
  localhost/Tailscale/single-user threat model and should be documented in
  `README.md`.

## v0.3.0 Session Hygiene

Add explicit cleanup commands.

Planned commands:

- `0pty rm NAME`
- `0pty prune`
- `0pty logs NAME`

Rules:

- `list` should not auto-delete dead sessions.
- `rm NAME` removes one session record after confirming it is not alive.
- `prune` removes confirmed-dead session records.
- `logs NAME` prints or tails the session log path from the session file.

## v0.4.0 Install And Shell Polish

Make the tool easier to install and use interactively.

Planned work:

- `make install PREFIX=...`
- Install both `0pty` and `0pty-server`.
- Optional shell completions.
- README install section.
- Better examples for SSH/Tailscale workflows.

## Later

### `0pty-supervisor`

Keep the PTY server dumb and auditable. Put automatic restart policy in a
separate supervisor binary or service.

Possible future session fields:

- `restart_policy=manual`
- `restart_policy=on-crash`
- `restart_policy=always`

The default remains `manual`.

### Disk-Persisted Server Scrollback

Persist server-side ring-buffer snapshots so a server restart can still provide
recent scrollback history. This does not preserve the running process; it only
preserves context.

### Protocol Hardening

- Add explicit `OK` / `ERROR` control responses where useful.
- Consider a session nonce in attach handshakes to avoid stale session files
  accidentally attaching to a reused port.
- Keep framing small and auditable.

