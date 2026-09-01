#!/usr/bin/env python3
"""Backfill real tags into playlists that were built by a directory scan.

A directory scan stages tracks fast by design: it takes the title from the file
name and writes "Unknown artist" / "Unknown album" rather than probing every
file (see scan_worker in backend/app.c). That leaves a library with no real
metadata even when the files are fully tagged.

This reads the actual tags with ffprobe -- one ssh round trip per remote host,
not one per file -- and rewrites title/artist/album in place.

  ./scripts/retag-library.py                 # dry run: show what would change
  ./scripts/retag-library.py --apply         # write it
  ./scripts/retag-library.py --plain-title   # "Banana Co." not "Banana Co. (16)"

Stop the backend first when applying, so it does not write over the result:
  systemctl --user stop leecher-media.service
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from collections import defaultdict

DEFAULT_LIBRARY = os.path.join(
    os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")),
    "leecher-media", "library")

# One remote loop, fed the paths on stdin so neither argv length nor quoting
# can bite. The marker keeps each file's tags attached to its path.
REMOTE = (
    'while IFS= read -r f; do '
    'printf "===FILE\\t%s\\n" "$f"; '
    'ffprobe -v error -show_entries format_tags -of default=noprint_wrappers=1 -- "$f" 2>/dev/null; '
    'done'
)
SSH = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "LogLevel=ERROR",
       "-o", "ControlMaster=auto", "-o", "ControlPath=~/.ssh/cm-retag-%C",
       "-o", "ControlPersist=60"]


def parse_tags(block):
    """ffprobe 'TAG:Key=value' lines -> a lowercased dict."""
    tags = {}
    for line in block.splitlines():
        if not line.startswith("TAG:") or "=" not in line:
            continue
        key, _, value = line[4:].partition("=")
        tags[key.strip().lower()] = value.strip()
    return tags


def probe_remote(user, host, paths):
    out = {}
    if not paths:
        return out
    proc = subprocess.run(SSH + ["--", f"{user}@{host}", REMOTE],
                          input="\n".join(paths) + "\n",
                          capture_output=True, text=True, timeout=600)
    current = None
    buffer = []
    for line in proc.stdout.splitlines():
        if line.startswith("===FILE\t"):
            if current is not None:
                out[current] = parse_tags("\n".join(buffer))
            current = line[len("===FILE\t"):]
            buffer = []
        else:
            buffer.append(line)
    if current is not None:
        out[current] = parse_tags("\n".join(buffer))
    return out


def probe_local(paths):
    out = {}
    for path in paths:
        if not os.path.exists(path):
            continue
        try:
            proc = subprocess.run(
                ["ffprobe", "-v", "error", "-show_entries", "format_tags",
                 "-of", "default=noprint_wrappers=1", "--", path],
                capture_output=True, text=True, timeout=30)
            out[path] = parse_tags(proc.stdout)
        except (OSError, subprocess.SubprocessError):
            pass
    return out


AUDIO_EXT = re.compile(r"\.(flac|mp3|ogg|wav|m4a|aac|opus|wma)$", re.I)


def clean_title(title, artist, number):
    """Some files carry a sloppy Title tag -- the file name with its extension,
    sometimes with the track number or artist still glued on. Strip only what we
    can prove is redundant, so a song genuinely called "99 Problems" survives."""
    title = AUDIO_EXT.sub("", title.strip())
    # A trailing " (NN)" we (or a previous run) appended: drop it so re-running
    # does not stack "(01) (01)".
    title = re.sub(r"\s*\((\d{1,3})\)\s*$", "", title)
    # A leading track number, only when it is the very number this track has.
    if number is not None:
        match = re.match(r"\s*0*(\d{1,3})\s*[-._ ]\s*(.+)$", title)
        if match and int(match.group(1)) == number and match.group(2).strip():
            title = match.group(2).strip()
    # A leading "Artist - ", only when it is this track's artist.
    if artist:
        prefix = artist.strip().lower() + " - "
        if title.lower().startswith(prefix) and title[len(prefix):].strip():
            title = title[len(prefix):].strip()
    return title.strip()


def track_number(tags, fallback_name):
    """Tag track numbers show up as "6" or "6/12"; fall back to a leading
    number in the file name, which is how these libraries are usually laid out."""
    raw = tags.get("track") or tags.get("tracknumber") or ""
    match = re.match(r"\s*(\d+)", raw)
    if match:
        return int(match.group(1))
    match = re.match(r"\s*(\d+)\s*[-. ]", os.path.basename(fallback_name))
    return int(match.group(1)) if match else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library", default=DEFAULT_LIBRARY)
    ap.add_argument("--apply", action="store_true", help="write changes (default: dry run)")
    ap.add_argument("--plain-title", action="store_true",
                    help='use the bare tag title, without the " (NN)" track suffix')
    args = ap.parse_args()

    if not os.path.isdir(args.library):
        sys.exit(f"No library directory at {args.library}")

    files = sorted(f for f in os.listdir(args.library) if f.endswith(".json"))
    if not files:
        sys.exit(f"No playlists in {args.library}")

    # Gather every source across every playlist, then probe each host once.
    remote, local = defaultdict(set), set()
    loaded = {}
    for name in files:
        path = os.path.join(args.library, name)
        try:
            loaded[name] = json.load(open(path))
        except (OSError, ValueError) as exc:
            print(f"  skipping {name}: {exc}")
            continue
        for track in loaded[name].get("tracks", []):
            for src in track.get("sources", []):
                if src.get("kind") in ("ssh", "network") and src.get("PATH"):
                    remote[(src.get("USERNAME"), src.get("IP"))].add(src["PATH"])
                elif src.get("kind") == "local" and src.get("PATH"):
                    local.add(src["PATH"])

    tags = {}
    for (user, host), paths in remote.items():
        print(f"probing {len(paths)} file(s) on {user}@{host} (one connection)...")
        try:
            tags.update(probe_remote(user, host, sorted(paths)))
        except (OSError, subprocess.SubprocessError) as exc:
            print(f"  failed: {exc}")
    if local:
        print(f"probing {len(local)} local file(s)...")
        tags.update(probe_local(sorted(local)))

    changed_total = 0
    for name, data in loaded.items():
        changed = 0
        for track in data.get("tracks", []):
            src = (track.get("sources") or [{}])[0]
            got = tags.get(src.get("PATH") or "")
            if not got:
                continue
            title = got.get("title")
            if not title:
                continue
            artist = got.get("artist") or got.get("album_artist") or ""
            number = track_number(got, src.get("PATH") or "")
            title = clean_title(title, artist, number)
            if not title:
                continue
            if not args.plain_title and number is not None:
                title = f"{title} ({number:02d})"
            new = {"title": title,
                   "artist": artist or track.get("artist"),
                   "album": got.get("album") or track.get("album")}
            if all(track.get(k) == v for k, v in new.items()):
                continue
            if changed < 5:
                print(f"  {name}: {track.get('title')!r}\n"
                      f"      -> {new['title']!r}  |  {new['artist']}  |  {new['album']}")
            track.update(new)
            changed += 1
        if changed:
            print(f"  {name}: {changed} track(s) updated"
                  + (f" ({changed - 5} more not shown)" if changed > 5 else ""))
            changed_total += changed
            if args.apply:
                target = os.path.join(args.library, name)
                tmp = target + ".retag.tmp"
                with open(tmp, "w") as fh:
                    json.dump(data, fh, ensure_ascii=False)
                    fh.write("\n")
                os.replace(tmp, target)

    if not changed_total:
        print("Nothing to change.")
    elif args.apply:
        print(f"\nWrote {changed_total} track(s).")
    else:
        print(f"\nDry run: {changed_total} track(s) would change. Re-run with --apply.")


main()
