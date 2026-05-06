# SSH Port-Forwarding Workflow

Use 0pty over SSH by keeping `0pty-server` bound to `127.0.0.1` on your dev
box and forwarding the port to your local machine. No new ports are exposed to
the internet.

## Quick one-shot tunnel

```sh
# on your laptop: forward local port 6077 to the dev box's localhost:6077
ssh -L 6077:127.0.0.1:6077 user@dev-box -N &

# attach as if the server were local
0pty connect claude01
# or raw endpoint:
bin/0pty 127.0.0.1:6077
```

`-N` means "don't run a remote command, just hold the tunnel". Close it with
`fg` + Ctrl-C, or `kill %1` if it's backgrounded.

## Persistent tunnel via ~/.ssh/config

Add a stanza to `~/.ssh/config` on your laptop:

```
Host dev-box
    HostName your.server.example.com
    User     youruser
    LocalForward 6077 127.0.0.1:6077
    ServerAliveInterval 30
    ServerAliveCountMax 3
    ExitOnForwardFailure yes
```

Then open the tunnel:

```sh
ssh dev-box -N
```

Whenever this connection is alive, `bin/0pty 127.0.0.1:6077` (or
`0pty connect NAME`) works on your laptop.

## Auto-reconnecting tunnel with autossh

`autossh` restarts the SSH tunnel automatically if it drops:

```sh
autossh -M 0 -f -N dev-box
```

`-M 0` disables the autossh echo port (rely on `ServerAliveInterval` instead).
`-f` backgrounds the process.

## systemd user service for the tunnel

Create `~/.config/systemd/user/0pty-tunnel.service` on your laptop:

```ini
[Unit]
Description=SSH tunnel to 0pty on dev-box
After=network-online.target

[Service]
ExecStart=/usr/bin/autossh -M 0 -N dev-box
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
```

Enable it:

```sh
systemctl --user daemon-reload
systemctl --user enable --now 0pty-tunnel
```

## Security note

The server stays bound to `127.0.0.1` on the remote machine. Only someone with
SSH access to the box can reach it. No extra firewall rules needed.
