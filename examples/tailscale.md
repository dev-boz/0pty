# Tailscale Workflow

Use 0pty with Tailscale so you can reach your agent session from any device on
your tailnet — laptop, phone, or a second machine — without opening any ports
to the internet.

## On the dev box

Bind `0pty-server` to the Tailscale interface instead of localhost:

```sh
# find your Tailscale IP
tailscale ip -4

# start a persistent claude session on that interface
0pty claude01 start claude --resume
```

`0pty start` picks a free localhost port by default. To bind directly to the
Tailscale address for raw-endpoint use:

```sh
bin/0pty-server -b 100.x.y.z:6077 -- claude --resume
```

The server is single-binary with no daemon dependency; run it however you like.

## From any other Tailscale device

```sh
# named-session shorthand (if you used 0pty start on the dev box)
0pty connect claude01        # by name — requires the session file to be synced
                              # or use the raw endpoint form:
bin/0pty 100.x.y.z:6077
```

If you use named sessions from multiple machines, copy or sync
`~/.0pty/sessions/claude01` to the connecting device so `0pty connect claude01`
can resolve the host and port.

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
Enable the optional shared token (`-t TOKEN`) if any tailnet device should not
have unconditional access to the PTY.
