#!/usr/bin/env bash
#
# e2e.sh - end to end check of the daemon against tools/fake-discord.
#
# Usage: tests/e2e.sh <path-to-GTAVI-Discord> <path-to-fake-discord>
#
# Verifies the whole chain: game process appears -> activity published with a
# timer, game process exits -> activity cleared, --status/--mode/--stop control
# channel works.  Every wait is bounded and polls, so a slow CI runner cannot
# turn it into a flaky test.
set -uo pipefail

EXE=${1:?path to GTAVI-Discord}
FAKE=${2:?path to fake-discord}

tmp=$(mktemp -d)
daemon_pid=""
game_pid=""
fake_pid=""

cleanup() {
    set +e
    [ -n "$daemon_pid" ] && kill "$daemon_pid" 2>/dev/null
    [ -n "$game_pid" ] && kill "$game_pid" 2>/dev/null
    [ -n "$fake_pid" ] && kill "$fake_pid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

fail() {
    echo "E2E FAIL: $*" >&2
    echo "--- presence.log ---" >&2
    cat "$tmp/presence.log" 1>&2 2>/dev/null || true
    echo "--- fake discord ---" >&2
    cat "$tmp/fake.log" 1>&2 2>/dev/null || true
    exit 1
}

# wait_for <timeout-seconds> <description> <command...>
wait_for() {
    local timeout=$1
    local description=$2
    shift 2
    local deadline=$((SECONDS + timeout))

    while [ $SECONDS -lt $deadline ]; do
        if "$@" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    fail "$description"
}

log_contains() {
    grep -q -- "$1" "$tmp/fake.log"
}

sock="$tmp/discord-ipc-0"

"$FAKE" "$sock" >"$tmp/fake.log" 2>&1 &
fake_pid=$!
wait_for 10 "fake discord did not create its socket" test -S "$sock"

# A stand-in for GTA5.exe: the detector matches on the process image name.
cp "$(command -v sleep)" "$tmp/GTA5.exe" || fail "cannot copy sleep"
cat >"$tmp/config.ini" <<EOF
[discord]
client_id = 123456789012345678
mode = pc

[activity]
large_image = gta6
large_text = Grand Theft Auto VI
small_image =
small_text = PlayStation 5

[playstation]
platform = ps5

[detection]
poll_ms = 200
processes = GTA5.exe

[log]
level = debug
EOF

export XDG_RUNTIME_DIR="$tmp"
export GTAVI_DISCORD_IPC="$sock"

# 1. the daemon connects to Discord and publishes nothing while the game is not
#    running
"$EXE" --config "$tmp/config.ini" &
daemon_pid=$!
wait_for 20 "no handshake sent to Discord" log_contains "HANDSHAKE"
wait_for 20 "no READY from the fake Discord" log_contains "READY sent"
sleep 1
log_contains '"activity"' && fail "activity published while the game is not running"

# 2. start the "game": the activity must appear with a timer and artwork
"$tmp/GTA5.exe" 60 &
game_pid=$!
wait_for 20 "activity was not published when the game started" log_contains '"activity":{"type":0'
log_contains '"start":' || fail "elapsed timer is missing"
log_contains '"large_image":"gta6"' || fail "artwork is missing"

# 3. no state/details, ever
log_contains '"state"' && fail "activity contains state"
log_contains '"details"' && fail "activity contains details"

# 4. status command
status=$("$EXE" --status) || fail "--status failed"
case "$status" in
    *"playing=yes"*) ;;
    *) fail "--status did not report the game: $status" ;;
esac

# 5. switch to the playstation mode: the platform field must appear
"$EXE" --mode playstation >/dev/null || fail "--mode playstation failed"
wait_for 20 "playstation mode did not send the platform tag" log_contains '"platform":"ps5"'
grep -q "mode = playstation" "$tmp/config.ini" || fail "mode was not persisted to config.ini"

# 6. stop the game: the activity must be cleared
kill "$game_pid"
wait "$game_pid" 2>/dev/null || true
game_pid=""
wait_for 20 "activity was not cleared after the game exited" log_contains '"activity":null'

# 7. stop the daemon
stop_reply=$("$EXE" --stop) || fail "--stop failed"
case "$stop_reply" in
    *stopped*) ;;
    *) fail "--stop returned: $stop_reply" ;;
esac
sleep 1
if [ -d "/proc/$daemon_pid" ]; then
    fail "daemon is still running after --stop"
fi
daemon_pid=""

# 8. the daemon logged its shutdown
wait_for 10 "daemon did not log its shutdown" grep -q "stopped" "$tmp/presence.log"

echo "e2e: ok"
