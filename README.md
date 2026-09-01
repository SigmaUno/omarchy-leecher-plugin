# Leecher Music for Omarchy

An [Omarchy](https://omarchy.org/) bar widget for
[Leecher Music Player](https://github.com/SigmaUno/music-leecher). It displays
the current song, artist, album art, and playback progress, with controls for
play/pause, seeking, previous/next, autoplay, library selection, local-song
adding, metadata editing, and track removal.

## Requirements

- Omarchy 4.0 or newer, with Quickshell plugin support.
- Build dependencies: `make`, a C compiler, `pkg-config`, SDL2 development
  files, and libsndfile development files. On Omarchy, install the packages
  with `omarchy pkg add sdl2 libsndfile`.
- Runtime tools for every Leecher source type: `curl`, `ffmpeg`, `ssh`, and
  `ssh-agent`. `zenity` is optional and enables graphical file selection.
- A user systemd session. The included installer builds and starts the Leecher
  backend. The widget reads its local runtime state from
  `$XDG_RUNTIME_DIR/leecher/` (or `/tmp/leecher-<uid>/` when the runtime
  directory is unavailable).

The widget is only a controller and display for that backend; it does not play
music on its own.

## Install

Clone the repository, build the bundled backend, and register the plugin:

```sh
git clone https://github.com/SigmaUno/omarchy-leecher-plugin.git
cd omarchy-leecher-plugin
./install-backend.sh
omarchy plugin add "$PWD" --enable
```

Omarchy will ask to confirm installation because plugins run as code in the
long-lived desktop shell. Review the source before accepting it. The widget is
placed in the left bar section by default; move it later with Omarchy's bar
controls if desired. The installer uses only user-owned locations and does not
require `sudo`.

## Update

```sh
./update.sh
```

Pulls `main`, rebuilds and reinstalls the backend, redeploys the widget, and
restarts the shell so the new QML is loaded. Editing this checkout on its own
changes nothing about the running plugin: the backend runs from a copy in
`$XDG_DATA_HOME/leecher-media` and the widget from a copy in
`$XDG_CONFIG_HOME/omarchy/plugins/leecher.media/`.

Pass `--no-pull` to deploy the working tree as-is, or `--no-restart` to leave
the shell running. The pull is refused if the checkout has uncommitted changes.

## Use

- Click the bar widget to open the player panel.
- Click the play icon in the bar to toggle playback.
- Use the library button in the panel to select, add, edit, or remove tracks.
  Choose **Add local song** and enter the full path to an audio file; its tags
  are imported automatically, with the filename used when tags are absent.
- The panel holds multiple playlists. The strip next to "Library" lists each one;
  click a tab to browse it, or the framed **+** to type a new playlist name
  inline. Playing a track from a tab switches playback to that playlist; the tab
  it left keeps a half-tint while it is still the one playing.
- Right-click the widget to hide it or collapse it to a restorable handle.

## Remote playback

SSH and network sources stream over a single multiplexed `ssh` connection per
host (OpenSSH `ControlMaster`), so only the first track of a session pays the
connection handshake; later tracks, their metadata, and their cover art reuse
it. The control sockets live in `$XDG_RUNTIME_DIR/leecher/ssh/` and are removed
on exit. Playback of a remote FLAC begins as soon as its header and a short
buffer have arrived rather than after the whole file downloads; other formats
still download fully before playback starts.

## Remove

```sh
omarchy plugin remove leecher.media
./uninstall-backend.sh
```

Removing the plugin does not remove your music library. The backend uninstall
stops its user service and removes installed binaries, while preserving your
playlists in `$XDG_DATA_HOME/leecher-media/library/` (an existing single-file
`library.json` from an older version is migrated to `library/home.json` on the
first run).

## Privacy and security

The widget has no network access or credentials of its own. It reads the local
Leecher status and library files and writes playback commands to the local
Leecher control file. Like every Omarchy plugin, it runs unsandboxed inside the
user's shell process; install it only from a source you trust.

## License

[MIT](LICENSE)
