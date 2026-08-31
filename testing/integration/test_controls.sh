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
    # Poll a good while: on a loaded CI runner the backend can take a moment to
    # get around to the control file (it also does blocking fetch-thread joins).
    tries=250
    while [ "$tries" -gt 0 ]; do
        [ -f "$D/status.json" ] && \
            [ "$(jq -r '.cmd_id' "$D/status.json" 2>/dev/null)" = "$cmd_id" ] && return 0
        sleep 0.02; tries=$((tries - 1))
    done
    printf '  (command "%s" was not acknowledged)\n' "$1" >&2
    return 1
}

# ---- launch --------------------------------------------------------------
launch() {
    XDG_RUNTIME_DIR="$run" SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy \
        "$app" --headless "$lib" </dev/null >/dev/null 2>>"$work/app.log" &
    APP_PID=$!
}
stop_app() { kill -TERM "$APP_PID" 2>/dev/null; wait "$APP_PID" 2>/dev/null; }

launch
wait_for '.title != "" and .is_playing == true' "autoplay starts a track" || exit 1

# The fixture ships a single-file library.json; the backend migrates it into a
# library/ directory as home.json. Everything below asserts against the real
# viewed-playlist file the status reports.
libfile=$(jq -r '.library' "$D/status.json")
libdir=$(dirname "$libfile")
jq -e '.playlists == ["home", "*"] and .viewed_playlist == "home" and .playing_playlist == "home"' \
    "$D/status.json" >/dev/null 2>&1 \
    && pass "legacy library.json migrated; auto-collect * seeded" \
    || { fail "legacy library.json migrated; auto-collect * seeded"; jq '{playlists,viewed_playlist,playing_playlist,library}' "$D/status.json"; }
[ -f "$libdir/home.json" ] && [ -f "$libdir/*.json" ] \
    && pass "home.json and *.json both exist" || fail "home.json and *.json both exist"

# ---- transport ---------------------------------------------------------------
send "play_pause"
wait_for '.is_playing == false' "play_pause pauses"
send "play_pause"
wait_for '.is_playing == true' "play_pause resumes"

send "next"
wait_for '.track_index == 1' "next advances track"
send "previous"
wait_for '.track_index == 0' "previous goes back"
send "previous"
wait_for '.track_index == 2' "previous wraps to the last track"
send "next"
wait_for '.track_index == 0' "next wraps to the first track"

send "seek 500"
wait_for '.position_ms >= 400' "seek moves the position"
send "seek -1000"
wait_for '.position_ms < 500' "negative seek clamps to 0"

send "play 99"
wait_for '.status | test("[Cc]annot|[Cc]ould not|range")' "out-of-range play is rejected"
wait_for '.is_playing == true' "playback continues after a bad play"

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
python3 - "$libfile" "$work" <<'PY'
import json, sys
f, d = sys.argv[1], sys.argv[2]
lib = json.load(open(f))
lib["tracks"].append({
    "title": "Broken", "artist": "x", "album": "y",
    "sources": [{"kind": "local", "PATH": f"{d}/does-not-exist.wav",
                 "USERNAME": None, "URL": None, "IP": None}],
})
json.dump(lib, open(f, "w"))
PY
broken_idx=$(jq '.tracks | length - 1' "$libfile")
send "play $broken_idx"
wait_for '.status | test("Skipped|Could not play")' "broken source is reported, not silent"

# ---- library mutation mid-playback keeps playing (#11) -------------------
send "play 0"
wait_for '.track_index == 0 and .is_playing == true' "back on a real track"
before=$(jq '.tracks | length' "$libfile")
"$here/make_fixture.sh" "$work/extra" >/dev/null
add=$work/extra/track0.wav
send "add_local $add"
wait_for '.is_playing == true' "playback survives a mid-playback add"
wait_for '.status | test("[Ii]mport")' "add reported"
after=$(jq '.tracks | length' "$libfile")
[ "$after" -gt "$before" ] && pass "added track landed in the library" \
    || fail "added track landed in the library"
# position must still be advancing after the reload settles
q1=$(jq '.position_ms' "$D/status.json"); sleep 0.6
q2=$(jq '.position_ms' "$D/status.json")
[ "$q2" -gt "$q1" ] && pass "position keeps advancing after the mutation" \
    || fail "position keeps advancing after the mutation"

# ---- editing a track's fields --------------------------------------------------
send "set_fields 0 Renamed%20Track NewArtist NewAlbum"
wait_for '.status | test("[Uu]pdate")' "set_fields reported"
jq -e '.tracks[0].title == "Renamed Track" and .tracks[0].artist == "NewArtist"' \
    "$libfile" >/dev/null 2>&1 \
    && pass "set_fields wrote the new title/artist" \
    || { fail "set_fields wrote the new title/artist"; jq '.tracks[0]' "$libfile"; }

# ---- behaviour at the end of a track -----------------------------------------
# playing() asserts a track is actually committed (fetch done) so a follow-up
# seek isn't dropped as "no audio loaded".
seek_near_end() {   # $1 = expected track index
    wait_for '.track_index == '"$1"' and .duration_ms > 0 and (.status | test("Playing"))' \
        "track $1 is committed and playing"
    dur=$(jq '.duration_ms' "$D/status.json")
    send "seek $((dur - 800))"
    wait_for '.position_ms > '"$((dur - 1500))" "seeked to the end of track $1"
}

send "autoplay on"; send "repeat off"; send "play 0"
seek_near_end 0
wait_for '.track_index == 1' "autoplay advances at end of track" 80

send "autoplay off"; send "play 0"
seek_near_end 0
wait_for '.is_playing == false and .track_index == 0' "autoplay off: playback stops at end, no advance" 80

send "autoplay on"; send "repeat one"; send "play 2"
seek_near_end 2
wait_for '.track_index == 2 and .position_ms < 5000' "repeat-one replays the same track from the start" 80
send "repeat off"

# ---- shuffle picks a variety of tracks --------------------------------------
send "shuffle on"
seen=""
for _ in 1 2 3 4 5 6 7 8; do
    send "next"
    sleep 0.2
    t=$(jq -r '.track_index' "$D/status.json")
    case "$seen" in *" $t "*) : ;; *) seen="$seen $t " ;; esac
done
send "shuffle off"
distinct=$(printf '%s\n' $seen | wc -w)
[ "$distinct" -ge 2 ] && pass "shuffle reaches more than one track ($distinct of 3)" \
    || fail "shuffle reaches more than one track (only $distinct)"

# ---- resume the song + position across a restart ------------------------------
send "play 1"
wait_for '.track_index == 1 and (.status | test("Playing"))' "on track 1 for the resume test"
send "seek 12000"
wait_for '.position_ms >= 11000' "seeked into track 1"
send "play_pause"
wait_for '.is_playing == false' "paused before restart"
sleep 0.5   # let the pause + resume-file write settle before SIGTERM
stop_app

resume="$libdir/.resume.json"
[ -f "$resume" ] && pass "resume.json was written" || fail "resume.json was written"
jq -e '.playlist == "home" and .track_index == 1 and .position_ms >= 8000 and .is_playing == false' "$resume" >/dev/null 2>&1 \
    && pass "resume.json holds playlist / track / position / paused state" \
    || { fail "resume.json holds playlist / track / position / paused state"; cat "$resume"; }

launch
wait_for '.track_index == 1 and .is_playing == false and .position_ms >= 6000 and .position_ms < 18000' \
    "restart resumes the same track, position and paused state"
before_pos=$(jq '.position_ms' "$D/status.json"); sleep 0.7
wait_for '.is_playing == false' "resumed track stays paused"
send "play_pause"
wait_for '.is_playing == true' "play resumes after the restart"

# ---- multiple playlists -----------------------------------------------------
send "play 0"
wait_for '.track_index == 0 and .is_playing == true' "playing home track 0"

send "playlist_new roadtrip"
wait_for '.playlists | index("roadtrip") != null' "playlist_new creates a playlist"
wait_for '.viewed_playlist == "roadtrip"' "new playlist becomes the viewed one"
[ -f "$libdir/roadtrip.json" ] && pass "playlist file written to library/" \
    || fail "playlist file written to library/"
# playback keeps running over home while roadtrip is merely viewed
jq -e '.playing_playlist == "home" and .title != ""' "$D/status.json" >/dev/null 2>&1 \
    && pass "playback stays on home while viewing roadtrip" \
    || { fail "playback stays on home while viewing roadtrip"; jq '{playing_playlist,viewed_playlist,title}' "$D/status.json"; }

send "playlist_new bad/name"
wait_for '.status | test("Invalid playlist name")' "an invalid playlist name is rejected"
jq -e '.playlists | index("bad/name") == null' "$D/status.json" >/dev/null 2>&1 \
    && pass "the rejected name creates no file" || fail "the rejected name creates no file"

send "playlist nope"
wait_for '.status | test("No such playlist")' "switching to a missing playlist is rejected"

# add a track to roadtrip, then play it: playback must move to roadtrip
send "playlist roadtrip"
wait_for '.viewed_playlist == "roadtrip"' "back to viewing roadtrip"
"$here/make_fixture.sh" "$work/rt" >/dev/null
send "add_local $work/rt/track0.wav"
wait_for '.status | test("[Ii]mport")' "track added to roadtrip"

# the auto-collect * playlist mirrors every add
star="$libdir/*.json"
jq -e '[.tracks[].sources[].PATH] | index("'"$work/rt/track0.wav"'") != null' "$star" >/dev/null 2>&1 \
    && pass "add is mirrored into the * playlist" \
    || { fail "add is mirrored into the * playlist"; jq '[.tracks[].sources[].PATH]' "$star"; }
send "add_local $work/rt/track0.wav"   # same file again
wait_for '.status | test("[Ii]mport")' "re-add of the same file reported"
[ "$(jq '[.tracks[].sources[] | select(.PATH == "'"$work/rt/track0.wav"'")] | length' "$star")" = "1" ] \
    && pass "* does not duplicate an already-collected source" \
    || fail "* does not duplicate an already-collected source"

send "play 0"
wait_for '.playing_playlist == "roadtrip" and .is_playing == true' \
    "playing a roadtrip track switches playback to roadtrip"

send "playlist home"
wait_for '.viewed_playlist == "home" and .playing_playlist == "roadtrip"' \
    "home can be viewed while roadtrip plays"
send "play 2"
wait_for '.playing_playlist == "home" and .track_index == 2' \
    "playing a home track switches playback back to home"

# --------------------------------------------------------------------------
echo
if [ "$fails" -eq 0 ]; then
    echo "integration: all checks passed"
else
    echo "integration: $fails failure(s)"
    echo "--- app.log ---"; cat "$work/app.log"
fi
exit $((fails > 0 ? 1 : 0))
