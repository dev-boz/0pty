# Tailscale Workflow

Use 0pty with Tailscale so you can reach your agent session from any device on
your tailnet — laptop, phone, or a second machine — without opening any ports
to the internet.

## On the dev box

For direct access from other devices, bind `0pty-server` to the Tailscale
interface instead of localhost:

```sh
# find your Tailscale IP
tailscale ip -4

# start a persistent claude session on that interface
bin/0pty-server -b 100.x.y.z:6077 -- claude --resume
```

Named `0pty NAME start` sessions currently pick a free localhost port. Use raw
endpoint mode for direct Tailscale access until named sessions grow an explicit
bind option.

The server is single-binary with no daemon dependency; run it however you like.

## From any other Tailscale device

```sh
bin/0pty 100.x.y.z:6077
```

If you prefer named sessions, keep them bound to localhost on the dev box and
use SSH forwarding as shown in `examples/ssh.md`.

## systemd on the dev box

Drop `systemd/0pty-server.service` into `~/.config/systemd/user/`, edit
`ExecStart` to use your Tailscale IP and the command you want to persist, then:

```sh
systemctl --user daemon-reload
systemctl --user enable --now 0pty-server
```

## Security note

Tailscale traffic is WireGuard-encrypted end-to-end. Binding to your Tailscale
address is safe for trusted devices on your tailnet. Do not bind to `0.0.0.0`.
Enable the optional shared token if any tailnet device should not have
unconditional access to the PTY:

```sh
export OPTY_TOKEN='choose-a-long-random-value'

# server
bin/0pty-server -b 100.x.y.z:6077 -t "$OPTY_TOKEN" -- claude --resume

# client
bin/0pty -t "$OPTY_TOKEN" 100.x.y.z:6077
```
