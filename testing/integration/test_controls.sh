#!/bin/bash
# Black-box integration test: spawn the headless backend against a fixture
# library and drive it through the control file, asserting on status.json.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
app="$root/backend/app"

command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 2; }

if [ ! -x "$app" ]; then
    echo "== building backend =="
    make -C "$root/backend" app || exit 2
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/leecher-it.XXXXXX")
# The backend only trusts an IPC dir it can create itself at 0700; give it one.
run="$work/run"; mkdir -p "$run"; chmod 700 "$run"
D="$run/leecher"

cleanup() {
    [ -n "${APP_PID:-}" ] && kill "$APP_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

lib=$("$here/make_fixture.sh" "$work/lib")

fails=0
pass() { printf '  ok   %s\n' "$1"; }
fail() { printf '  FAIL %s\n' "$1"; fails=$((fails + 1)); }

# Wait until status.json exists and satisfies the jq filter in $1.
wait_for() {
    filter=$1 desc=$2 tries=${3:-50}
    while [ "$tries" -gt 0 ]; do
        if [ -f "$D/status.json" ] && jq -e "$filter" "$D/status.json" >/dev/null 2>&1; then
            pass "$desc"; return 0
        fi
        sleep 0.1; tries=$((tries - 1))
    done
    printf '  status: %s\n' "$(cat "$D/status.json" 2>/dev/null || echo '<none>')"
    fail "$desc"; return 1
}

# The backend reads one line then deletes the control file, so a second write
# before it polls would clobber the first. Send, then wait for the echoed
# cmd_id before returning so callers can fire commands back to back.
cmd_id=0
send() {
    cmd_id=$((cmd_id + 1))
    printf '%d %s\n' "$cmd_id" "$1" > "$D/control"
    tries=100
    while [ "$tries" -gt 0 ]; do
        [ -f "$D/status.json" ] && \
            [ "$(jq -r '.cmd_id' "$D/status.json" 2>/dev/null)" = "$cmd_id" ] && return 0
        sleep 0.02; tries=$((tries - 1))
    done
    printf '  (command "%s" was not acknowledged)\n' "$1" >&2
    return 1
}

# ---- launch --------------------------------------------------------------
XDG_RUNTIME_DIR="$run" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
    "$app" --headless "$lib" 2>"$work/app.log" &
APP_PID=$!

wait_for '.title != "" and .is_playing == true' "autoplay starts a track" || exit 1

# ---- transport ---------------------------------------------------------------
send "play_pause"
wait_for '.is_playing == false' "play_pause pauses"
send "play_pause"
wait_for '.is_playing == true' "play_pause resumes"

send "next"
wait_for '.track_index == 1' "next advances track"
send "previous"
wait_for '.track_index == 0' "previous goes back"

send "seek 500"
wait_for '.position_ms >= 400' "seek moves the position"

# ---- modes -----------------------------------------------------------------
send "autoplay off"
wait_for '.autoplay == false' "autoplay off"
send "autoplay on"
wait_for '.autoplay == true' "autoplay on"

send "shuffle on"
wait_for '.shuffle == true' "shuffle on"
send "shuffle off"
wait_for '.shuffle == false' "shuffle off"

send "repeat one"
wait_for '.repeat_one == true' "repeat one"
send "repeat off"
wait_for '.repeat_one == false' "repeat off"

# ---- volume / mute -------------------------------------------------------
send "volume 30"
wait_for '.volume == 30' "volume set"
send "volume 500"
wait_for '.volume == 100' "volume clamps to 100"
send "mute on"
wait_for '.muted == true and .volume == 100' "mute keeps the volume value"
send "mute off"
wait_for '.muted == false' "unmute"

# ---- play queue ---------------------------------------------------------------
# autoplay would consume the queue itself when a track ends; hold it so the
# queue-management assertions are deterministic.
send "autoplay off"
wait_for '.autoplay == false' "autoplay held for queue tests"

send "queue 2"
send "queue 1"
wait_for '.queue == [2, 1]' "queue keeps order"
send "unqueue 2"
wait_for '.queue == [1]' "unqueue removes an entry"
send "queue_clear"
wait_for '.queue == []' "queue_clear empties"

# queued track plays before library order
send "previous"                      # make sure we're not already on track 2
wait_for '.track_index != 2' "reposition off track 2"
send "queue 2"
wait_for '.queue == [2]' "re-queue"
send "next"
wait_for '.track_index == 2' "next consumes the queue head"
wait_for '.queue == []' "queue drained after consumption"

send "autoplay on"
wait_for '.autoplay == true' "autoplay restored"

# ---- output device ---------------------------------------------------------
wait_for '(.outputs | type) == "array" and (.outputs | length) >= 1' "outputs are enumerated"
dev=$(jq -r '.outputs[0]' "$D/status.json")
send "output $dev"
if wait_for '.output != ""' "output device selected"; then
    jq -e --arg d "$dev" '.output == $d' "$D/status.json" >/dev/null 2>&1 \
        && pass "output matches the chosen device" \
        || fail "output matches the chosen device"
fi
send "output default"
wait_for '.output == ""' "output back to system default"
send "output No Such Device 12345"
wait_for '.status | test("No such output")' "unknown output rejected"

# ---- unknown command -------------------------------------------------------
send "frobnicate"
wait_for '.status | test("Unknown control command")' "unknown command reported"

# ---- autoplay skips a broken source --------------------------------------
python3 - "$work" <<'PY'
import json, sys
d = sys.argv[1]
lib = json.load(open(f"{d}/lib/library.json"))
lib["tracks"].append({
    "title": "Broken", "artist": "x", "album": "y",
    "sources": [{"kind": "local", "PATH": f"{d}/does-not-exist.wav",
                 "USERNAME": None, "URL": None, "IP": None}],
})
json.dump(lib, open(f"{d}/lib/library.json", "w"))
PY
broken_idx=$(jq '.tracks | length - 1' "$work/lib/library.json")
send "play $broken_idx"
wait_for '.status | test("Skipped|Could not play")' "broken source is reported, not silent"

# ---- library mutation mid-playback keeps playing (#11) -------------------
send "play 0"
wait_for '.track_index == 0 and .is_playing == true' "back on a real track"
before=$(jq '.tracks | length' "$work/lib/library.json")
"$here/make_fixture.sh" "$work/extra" >/dev/null
add=$work/extra/track0.wav
send "add_local $add"
wait_for '.is_playing == true' "playback survives a mid-playback add"
wait_for '.status | test("[Ii]mport")' "add reported"
after=$(jq '.tracks | length' "$work/lib/library.json")
[ "$after" -gt "$before" ] && pass "added track landed in the library" \
    || fail "added track landed in the library"
# position must still be advancing after the reload settles
q1=$(jq '.position_ms' "$D/status.json"); sleep 0.6
q2=$(jq '.position_ms' "$D/status.json")
[ "$q2" -gt "$q1" ] && pass "position keeps advancing after the mutation" \
    || fail "position keeps advancing after the mutation"

# --------------------------------------------------------------------------
echo
if [ "$fails" -eq 0 ]; then
    echo "integration: all checks passed"
else
    echo "integration: $fails failure(s)"
    echo "--- app.log ---"; cat "$work/app.log"
fi
exit $((fails > 0 ? 1 : 0))
