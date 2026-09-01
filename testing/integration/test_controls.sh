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

# Poll status.json against a jq filter without emitting a pass/fail line.
poll_state() {
    _t=${2:-50}
    while [ "$_t" -gt 0 ]; do
        [ -f "$D/status.json" ] && jq -e "$1" "$D/status.json" >/dev/null 2>&1 && return 0
        sleep 0.1; _t=$((_t - 1))
    done
    return 1
}

# Wait until status.json exists and satisfies the jq filter in $1.
wait_for() {
    filter=$1 desc=$2 tries=${3:-50}
    if poll_state "$filter" "$tries"; then pass "$desc"; return 0; fi
    printf '  status: %s\n' "$(cat "$D/status.json" 2>/dev/null || echo '<none>')"
    fail "$desc"; return 1
}

# The control file is written non-atomically (truncate then write) and the
# backend polls asynchronously, so on a loaded CI runner a single write can be
# missed and then clobbered by the next command. Re-issue the same line (same
# id, so the backend can't act on it twice) until the id is echoed back. The
# ack check reads the file with a shell `case` rather than forking jq every
# tick, so the loop stays tight.
cmd_id=0
send() {
    cmd_id=$((cmd_id + 1))
    attempt=0
    while [ "$attempt" -lt 5 ]; do
        printf '%d %s\n' "$cmd_id" "$1" > "$D/control"
        deadline=$(( $(date +%s) + 4 ))
        while [ "$(date +%s)" -lt "$deadline" ]; do
            if [ -f "$D/status.json" ]; then
                case "$(cat "$D/status.json" 2>/dev/null)" in
                    *"\"cmd_id\":$cmd_id}"*) return 0 ;;
                esac
            fi
            sleep 0.05
        done
        attempt=$((attempt + 1))
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

# A resent line (same id) must be acted on once, not toggled twice.
# The first copy goes through send(), which re-issues the SAME id until it is
# acked -- a raw single write here was the flake: the control file is written
# non-atomically and a loaded runner can miss the poll, so "first copy pauses"
# failed before the dedup behaviour was ever exercised. Re-issuing is safe
# precisely because of the dedup this test is checking.
send "play_pause"
wait_for '.is_playing == false' "resend: first copy pauses"
printf '%d play_pause\n' "$cmd_id" > "$D/control"   # identical id
sleep 0.5
jq -e '.is_playing == false' "$D/status.json" >/dev/null 2>&1 \
    && pass "resend with the same id is not run twice" \
    || { fail "resend with the same id is not run twice"; jq '{is_playing,cmd_id}' "$D/status.json"; }
send "play_pause"
wait_for '.is_playing == true' "playback resumed after the resend check"

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
# play_pause is a toggle and the just-restarted backend may still be settling
# the resumed track when the first one lands; toggle until it sticks.
resumed=0
for _ in 1 2 3; do
    send "play_pause"
    if poll_state '.is_playing == true' 40; then resumed=1; break; fi
done
[ "$resumed" = 1 ] && pass "play resumes after the restart" \
    || { fail "play resumes after the restart"; jq '{is_playing,status,cmd_id}' "$D/status.json"; }

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

# "*" is lazy: adding to roadtrip without viewing "*" leaves it untouched.
star="$libdir/*.json"
jq -e '[.tracks[]?.sources[]?.PATH] | index("'"$work/rt/track0.wav"'") == null' "$star" >/dev/null 2>&1 \
    && pass "* is not touched by an add to another playlist" \
    || { fail "* is not touched by an add to another playlist"; jq '[.tracks[]?.sources[]?.PATH]' "$star"; }
# viewing "*" rebuilds it from the union of the other playlists
send "add_local $work/rt/track0.wav"   # same file, second add
wait_for '.status | test("[Ii]mport")' "second add to roadtrip reported"
send "playlist *"
wait_for '.viewed_playlist == "*"' "switched to viewing *"
jq -e '[.tracks[].sources[].PATH] | index("'"$work/rt/track0.wav"'") != null' "$star" >/dev/null 2>&1 \
    && pass "viewing * collects the added track" \
    || { fail "viewing * collects the added track"; jq '[.tracks[].sources[].PATH]' "$star"; }
[ "$(jq '[.tracks[].sources[] | select(.PATH == "'"$work/rt/track0.wav"'")] | length' "$star")" = "1" ] \
    && pass "* de-duplicates a source added twice" \
    || fail "* de-duplicates a source added twice"

send "playlist roadtrip"
wait_for '.viewed_playlist == "roadtrip"' "back to roadtrip once more"
send "play 0"
wait_for '.playing_playlist == "roadtrip" and .is_playing == true' \
    "playing a roadtrip track switches playback to roadtrip"

send "playlist home"
wait_for '.viewed_playlist == "home" and .playing_playlist == "roadtrip"' \
    "home can be viewed while roadtrip plays"
send "play 2"
wait_for '.playing_playlist == "home" and .track_index == 2' \
    "playing a home track switches playback back to home"

# ---- directory scan -> INCOMING staging list -------------------------------
mkdir -p "$work/scan/sub"                          # a subdir the scan must ignore
: > "$work/scan/notes.txt"                         # a non-audio file to ignore
python3 - "$work/scan" <<'PY'
import sys, wave
d = sys.argv[1]
frames = b"\x00\x00" * (8000 * 3)
for name in ("Zeta", "Eta", "Theta"):
    with wave.open(f"{d}/{name}.wav", "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(frames)
PY
send "scan_local roadtrip $work/scan"
poll_state '.scanning == true or (.status | test("Scan"))' 30 >/dev/null
poll_state '.scanning == false and .viewed_playlist == "INCOMING >> roadtrip <<"' 100 \
    && pass "scan stages into INCOMING >> roadtrip <<" \
    || { fail "scan stages into INCOMING >> roadtrip <<"; jq '{scanning,scan_count,viewed_playlist,playlists,status}' "$D/status.json"; }
inc="$libdir/INCOMING >> roadtrip <<.json"
[ "$(jq '.tracks | length' "$inc" 2>/dev/null)" = "3" ] \
    && pass "3 audio files staged (subdir and .txt ignored)" \
    || { fail "3 audio files staged"; jq '.tracks | length' "$inc" 2>/dev/null; }
jq -e '.playlists == ["home", "roadtrip", "INCOMING >> roadtrip <<", "*"]' "$D/status.json" >/dev/null 2>&1 \
    && pass "staging tab sorts between the playlists and *" \
    || { fail "staging tab order"; jq '.playlists' "$D/status.json"; }

rt_before=$(jq '.tracks | length' "$libdir/roadtrip.json")
send "accept_incoming 2"
poll_state '.status | test("Accepted")' 40 >/dev/null
[ "$(jq '.tracks | length' "$libdir/roadtrip.json")" = "$((rt_before + 1))" ] \
    && pass "accept moves one track into the target" || fail "accept moves one track into the target"
[ "$(jq '.tracks | length' "$inc")" = "2" ] \
    && pass "accepted track leaves the staging list" || fail "accepted track leaves the staging list"

send "decline_incoming 0"
poll_state '.status | test("Declined")' 40 >/dev/null
[ "$(jq '.tracks | length' "$inc")" = "1" ] \
    && pass "decline drops a track from the staging list" || fail "decline drops from staging"
[ "$(jq '.tracks | length' "$libdir/roadtrip.json")" = "$((rt_before + 1))" ] \
    && pass "decline does not touch the target" || fail "decline does not touch the target"

send "accept_incoming 0"
poll_state '.viewed_playlist == "roadtrip"' 40 \
    && pass "clearing the staging list deletes it and returns to the target" \
    || { fail "clearing the staging list"; jq '{viewed_playlist,playlists}' "$D/status.json"; }
[ -f "$inc" ] && fail "empty staging file removed" || pass "empty staging file removed"

# --------------------------------------------------------------------------
echo
if [ "$fails" -eq 0 ]; then
    echo "integration: all checks passed"
else
    echo "integration: $fails failure(s)"
    echo "--- app.log ---"; cat "$work/app.log"
fi
exit $((fails > 0 ? 1 : 0))
