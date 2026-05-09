#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_HOME=$(mktemp -d)
SESSION="session-metadata-test"

cleanup() {
    "$ROOT_DIR/bin/0pty" stop "$SESSION" >/dev/null 2>&1 || true
    rm -rf "$TMP_HOME"
}
trap cleanup EXIT HUP INT TERM

export HOME=$TMP_HOME

LONG_ARG=
i=0
while [ "$i" -lt 1500 ]; do
    LONG_ARG="${LONG_ARG}x"
    i=$((i + 1))
done

SCRIPT='printf "first\n"
printf "second\n"
sleep 1'

printf '' | "$ROOT_DIR/bin/0pty" "$SESSION" start -- /bin/sh -c "$SCRIPT" "$LONG_ARG" >/dev/null 2>"$TMP_HOME/start.err"

SESSION_FILE="$HOME/.0pty/sessions/$SESSION.session"
test -f "$SESSION_FILE" || {
    echo "FAIL: session file not created"
    exit 1
}
test "$(grep -c '^argv2=' "$SESSION_FILE")" = "1" || {
    echo "FAIL: argv2 spilled across multiple lines"
    exit 1
}
grep -q '^argv3=' "$SESSION_FILE" || {
    echo "FAIL: long argv entry not stored"
    exit 1
}
if grep -q '^command=' "$SESSION_FILE"; then
    echo "FAIL: command display should not be stored"
    exit 1
fi
grep -q '^argv_encoding=escape$' "$SESSION_FILE" || {
    echo "FAIL: argv encoding marker not stored"
    exit 1
}
if grep -q '^printf "second' "$SESSION_FILE"; then
    echo "FAIL: session file leaked a raw newline from argv"
    exit 1
fi

i=0
while [ "$i" -lt 50 ]; do
    output=$("$ROOT_DIR/bin/0pty" list)
    if printf '%s\n' "$output" | grep -Eq "^$SESSION[[:space:]].*[[:space:]]dead[[:space:]]"; then
        break
    fi
    sleep 0.1
    i=$((i + 1))
done
[ "$i" -lt 50 ] || {
    echo "FAIL: session did not become dead in time"
    exit 1
}

# Keep stdin open while the restarted command emits its replay. 0pty detaches
# when stdin closes, matching non-interactive named-session behavior.
output=$(sleep 2 | "$ROOT_DIR/bin/0pty" restart "$SESSION")
printf '%s\n' "$output" | grep -q 'first' || {
    echo "FAIL: restart did not replay first line"
    exit 1
}
printf '%s\n' "$output" | grep -q 'second' || {
    echo "FAIL: restart did not replay second line"
    exit 1
}

echo "PASS: session metadata roundtrip"
