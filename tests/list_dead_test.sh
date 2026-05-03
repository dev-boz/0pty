#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_HOME=$(mktemp -d)

cleanup() {
    rm -rf "$TMP_HOME"
}
trap cleanup EXIT HUP INT TERM

export HOME=$TMP_HOME

output=$("$ROOT_DIR/bin/0pty" list)
printf '%s\n' "$output" | grep -q '^NAME' || {
    echo "FAIL: no-session list did not print header"
    exit 1
}

mkdir -p "$HOME/.0pty/sessions"
cat > "$HOME/.0pty/sessions/list-dead-test.session" <<'EOF'
name=list-dead-test
host=127.0.0.1
port=65534
log=/tmp/0pty-list-dead-test.log
restart_policy=manual
cwd=/tmp
EOF

output=$("$ROOT_DIR/bin/0pty" list)
printf '%s\n' "$output" | grep -q 'list-dead-test' || {
    echo "FAIL: dead session not listed"
    exit 1
}
printf '%s\n' "$output" | grep -q '127.0.0.1:65534' || {
    echo "FAIL: dead session endpoint not listed"
    exit 1
}
printf '%s\n' "$output" | grep -q 'dead' || {
    echo "FAIL: dead session not marked dead"
    exit 1
}
printf '%s\n' "$output" | grep -q 'manual' || {
    echo "FAIL: restart policy not listed"
    exit 1
}

echo "PASS: list dead session"
