#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_HOME=$(mktemp -d)
SESSION="stop-test"

cleanup() {
    "$ROOT_DIR/bin/0pty" stop "$SESSION" >/dev/null 2>&1 || true
    rm -rf "$TMP_HOME"
}
trap cleanup EXIT HUP INT TERM

export HOME=$TMP_HOME

( sleep 1 ) | "$ROOT_DIR/bin/0pty" "$SESSION" start -- /bin/sh -c 'printf "ready\n"; IFS= read line; test "$line" = /exit' >"$TMP_HOME/start.out" 2>"$TMP_HOME/start.err"

SESSION_FILE="$HOME/.0pty/sessions/$SESSION.session"
test -f "$SESSION_FILE" || {
    echo "FAIL: session file not created"
    exit 1
}
grep -q '^control_token=' "$SESSION_FILE" || {
    echo "FAIL: control token not stored"
    exit 1
}
grep -q '^graceful_input=/exit\\n$' "$SESSION_FILE" || {
    echo "FAIL: graceful input not stored"
    exit 1
}
test "$(stat -c %a "$SESSION_FILE")" = "600" || {
    echo "FAIL: session file permissions are not 0600"
    exit 1
}

"$ROOT_DIR/bin/0pty" stop "$SESSION" >"$TMP_HOME/stop.out" 2>&1
grep -q "stopped 0pty session $SESSION" "$TMP_HOME/stop.out" || {
    echo "FAIL: stop did not report success"
    cat "$TMP_HOME/stop.out"
    exit 1
}

output=$("$ROOT_DIR/bin/0pty" list)
printf '%s\n' "$output" | grep -q "$SESSION" || {
    echo "FAIL: stopped session not listed"
    exit 1
}
printf '%s\n' "$output" | grep -q 'dead' || {
    echo "FAIL: stopped session not marked dead"
    exit 1
}

echo "PASS: stop session"
