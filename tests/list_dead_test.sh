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
if "$ROOT_DIR/bin/0pty" connect > "$TMP_HOME/connect-empty.out" 2>&1; then
    echo "FAIL: empty connect succeeded"
    exit 1
fi
grep -q 'No 0pty sessions found' "$TMP_HOME/connect-empty.out" || {
    echo "FAIL: empty connect did not explain missing sessions"
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

if "$ROOT_DIR/bin/0pty" connect > "$TMP_HOME/connect-dead.out" 2>&1; then
    echo "FAIL: single dead connect succeeded"
    exit 1
fi
grep -q "Session list-dead-test is dead. Run '0pty restart list-dead-test' to bring it back." "$TMP_HOME/connect-dead.out" || {
    echo "FAIL: single dead connect did not print restart guidance"
    exit 1
}

cat > "$HOME/.0pty/sessions/list-dead-test-2.session" <<'EOF'
name=list-dead-test-2
host=127.0.0.1
port=65533
log=/tmp/0pty-list-dead-test-2.log
restart_policy=manual
cwd=/tmp
EOF

if "$ROOT_DIR/bin/0pty" connect > "$TMP_HOME/connect-multiple.out" 2>&1; then
    echo "FAIL: multi-session connect succeeded"
    exit 1
fi
grep -q 'list-dead-test' "$TMP_HOME/connect-multiple.out" || {
    echo "FAIL: multi-session connect did not print session list"
    exit 1
}
grep -q 'list-dead-test-2' "$TMP_HOME/connect-multiple.out" || {
    echo "FAIL: multi-session connect did not print all sessions"
    exit 1
}
grep -q 'Multiple 0pty sessions found. Choose one with: 0pty connect NAME' "$TMP_HOME/connect-multiple.out" || {
    echo "FAIL: multi-session connect did not request explicit choice"
    exit 1
}

echo "PASS: list dead session"
