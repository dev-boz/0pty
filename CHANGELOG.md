# Changelog

All notable changes to this project are documented here.

## [v0.2.0] — 2026-05-05

- Add graceful named-session shutdown: `0pty stop NAME` sends the session's stored `graceful_input` to the live PTY and waits for the server to exit cleanly
- Per-session control tokens stored in `~/.0pty/sessions` with 0600 permissions
- `TCP_NODELAY` enabled for lower interactive latency
- Atomic session file writes
- User-scoped log paths under `~/.0pty/logs`

## [v0.1.0] — 2026-05-03

- Core persistent PTY server (`0pty-server`) and client (`0pty`)
- Named sessions: `start`, `connect`, `list`, `restart`
- Smart `connect` with single-alive-session shorthand
- Ring buffer with sequence-number replay on reconnect
- Dependency-free build: C11, `pthread`, `util`
- Protocol regression test suite
