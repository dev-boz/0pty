# 0pty Roadmap

## v0.3.0 Session Hygiene

Add explicit cleanup commands.

Planned commands:

- `0pty rm NAME`
- `0pty prune`
- `0pty logs NAME`
- CLI support for setting `graceful_input` at session start

Rules:

- `list` should not auto-delete dead sessions.
- `rm NAME` removes one session record after confirming it is not alive.
- `prune` removes confirmed-dead session records.
- Named sessions already store a server log path under `~/.0pty/logs`.
- `logs NAME` prints or tails that existing path from the session file.
- `graceful_input` is already stored in session files and can be edited
  manually; a first-class CLI option is still pending.

## v0.4.0 Install And Shell Polish

Make the tool easier to install and use interactively.

Planned work:

- Optional shell completions.
- Named-session bind options for private/Tailscale interfaces.

## Later

### Access Modes

- Add a read-only attach mode for viewers who should receive PTY output without
  being able to send input or resize events.
- Add a one-connection-at-a-time mode for sessions that should refuse new
  attachers while one client is already active.

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

### Portability

- Keep Linux as the supported server target until non-Linux PTY behavior is
  deliberately tested.
- Validate WSL2 as a supported Linux environment.
- Investigate native macOS client support for attaching to Linux servers.
- Treat native Windows support as a separate design project, not a small build
  flag change.

### Protocol Hardening

- Add explicit `OK` / `ERROR` control responses where useful.
- Consider a session nonce in attach handshakes to avoid stale session files
  accidentally attaching to a reused port.
- Keep framing small and auditable.
