#!/bin/sh
# Generate a fixture library into $1 (a throwaway dir): a few short silent WAVs
# plus a library.json pointing at them. Prints the library path on stdout.
set -eu

dir=${1:?usage: make_fixture.sh <dir>}
mkdir -p "$dir"

python3 - "$dir" <<'PY'
import json, sys, wave

d = sys.argv[1]
rate, seconds = 8000, 20          # long enough that tests aren't racing playback
frames = b"\x00\x00" * (rate * seconds)

tracks = []
for i, (title, artist, album) in enumerate([
    ("Alpha", "Test Artist", "Fixtures"),
    ("Bravo", "Test Artist", "Fixtures"),
    ("Charlie", "Other Artist", "More Fixtures"),
]):
    path = f"{d}/track{i}.wav"
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(frames)
    tracks.append({
        "title": title, "artist": artist, "album": album,
        "sources": [{"kind": "local", "PATH": path,
                     "USERNAME": None, "URL": None, "IP": None}],
    })

with open(f"{d}/library.json", "w") as f:
    json.dump({"version": 1, "tracks": tracks}, f)
PY

echo "$dir/library.json"
