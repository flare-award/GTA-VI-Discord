#!/usr/bin/env bash
#
# e2e.sh - end to end check of the daemon against tools/fake-discord.
#
# Usage: tests/e2e.sh <path-to-gtavi-discord> <path-to-fake-discord>
#
# Verifies the whole chain: game process appears -> activity published with a
# timer, game process exits -> activity cleared, --status/--mode/--stop control
# channel works.
set -euo pipefail

EXE=${1:?path to gtavi-discord}
FAKE=${2:?path to fake-discord}

tmp=$(mktemp -d)
cleanup() {
    set +e
    [ -n "${daemon_pid:-}" ] && kill "$daemon_pid" 2>/dev/null
    [ -n "${game_pid:-}" ] && kill "$game_pid" 2>/dev/null
    [ -n "${fake_pid:-}" ] && kill "$fake_pid" 2>/dev/null
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

sock="$tmp/discord-ipc-0"

"$FAKE" "$sock" >"$tmp/fake.log" 2>&1 &
fake_pid=$!
for _ in $(seq 1 100); do
    [ -S "$sock" ] && break
    sleep 0.05
done
[ -S "$sock" ] || fail "fake discord did not create its socket"

# A stand-in for GTA5.exe: the detector matches on the process image name.
cp "$(command -v sleep)" "$tmp/GTA5.exe"
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

# 1. game not running yet: nothing may be published
"$EXE" --config "$tmp/config.ini" &
daemon_pid=$!
sleep 1.5
grep -q "HANDSHAKE" "$tmp/fake.log" || fail "no handshake sent to Discord"
grep -q "\"activity\":{\"type\":0" "$tmp/fake.log" && fail "activity published while the game is not running"

# 2. start the "game"
"$tmp/GTA5.exe" 30 &
game_pid=$!
sleep 2
grep -q "\"activity\":{\"type\":0" "$tmp/fake.log" || fail "activity was not published when the game started"
grep -q "\"start\":" "$tmp/fake.log" || fail "elapsed timer is missing"
grep -q "\"large_image\":\"gta6\"" "$tmp/fake.log" || fail "artwork is missing"

# 3. the activity must not contain state/details
grep -q "\"state\"" "$tmp/fake.log" && fail "activity contains state"
grep -q "\"details\"" "$tmp/fake.log" && fail "activity contains details"

# 4. status command
out=$("$EXE" --status) || fail "--status failed"
case "$out" in
    *"playing=yes"*) ;;
    *) fail "--status did not report the game: $out" ;;
esac

# 5. switch to the playstation mode: the platform field must appear
"$EXE" --mode playstation >/dev/null || fail "--mode playstation failed"
sleep 3
grep -q "\"platform\":\"ps5\"" "$tmp/fake.log" || fail "playstation mode did not send the platform tag"
grep -q "mode = playstation" "$tmp/config.ini" || fail "mode was not persisted to config.ini"

# 6. stop the game: the activity must be cleared
kill "$game_pid"
wait "$game_pid" 2>/dev/null || true
game_pid=""
sleep 3
grep -q "\"activity\":null" "$tmp/fake.log" || fail "activity was not cleared after the game exited"

# 7. stop the daemon
out=$("$EXE" --stop) || fail "--stop failed"
case "$out" in
    *stopped*) ;;
    *) fail "--stop returned: $out" ;;
esac
sleep 1
if kill -0 "$daemon_pid" 2>/dev/null; then
    fail "daemon is still running after --stop"
fi
daemon_pid=""

# 8. the activity was cleared before exiting
grep -q "stopped" "$tmp/presence.log" || fail "daemon did not log its shutdown"

echo "e2e: ok"
