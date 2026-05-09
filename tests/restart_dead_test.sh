#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP_HOME=$(mktemp -d)
WORK_DIR="$TMP_HOME/work"
SESSION="restart-dead-test"

cleanup() {
    rm -rf "$TMP_HOME"
    rm -f "/tmp/0pty-$SESSION.log"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$WORK_DIR"
export HOME=$TMP_HOME

cd "$WORK_DIR"
printf '' | "$ROOT_DIR/bin/0pty" "$SESSION" start -- /bin/sh -c 'printf "first\n"; sleep 1'

SESSION_FILE="$HOME/.0pty/sessions/$SESSION.session"
test -f "$SESSION_FILE" || {
    echo "FAIL: session file not created"
    exit 1
}
grep -q '^cwd='"$WORK_DIR"'$' "$SESSION_FILE" || {
    echo "FAIL: cwd not stored"
    exit 1
}
grep -q '^argc=3$' "$SESSION_FILE" || {
    echo "FAIL: argc not stored"
    exit 1
}
grep -q '^argv0=/bin/sh$' "$SESSION_FILE" || {
    echo "FAIL: argv0 not stored"
    exit 1
}
grep -q '^argv1=-c$' "$SESSION_FILE" || {
    echo "FAIL: argv1 not stored"
    exit 1
}
grep -Fq 'argv2=printf "first\\n"; sleep 1' "$SESSION_FILE" || {
    echo "FAIL: argv2 not stored"
    exit 1
}
if grep -q '^command=' "$SESSION_FILE"; then
    echo "FAIL: command display should not be stored"
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
    echo "FAIL: restart did not replay command output"
    exit 1
}

echo "PASS: restart dead session"
