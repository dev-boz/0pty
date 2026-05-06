# 0pty Roadmap

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
