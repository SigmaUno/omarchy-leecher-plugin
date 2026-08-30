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

## Use

- Click the bar widget to open the player panel.
- Click the play icon in the bar to toggle playback.
- Use the library button in the panel to select, add, edit, or remove tracks.
  Choose **Add local song** and enter the full path to an audio file; its tags
  are imported automatically, with the filename used when tags are absent.
- Right-click the widget to hide it or collapse it to a restorable handle.

## Remove

```sh
omarchy plugin remove leecher.media
./uninstall-backend.sh
```

Removing the plugin does not remove your music library. The backend uninstall
stops its user service and removes installed binaries, while preserving
`$XDG_DATA_HOME/leecher-media/library.json`.

## Privacy and security

The widget has no network access or credentials of its own. It reads the local
Leecher status and library files and writes playback commands to the local
Leecher control file. Like every Omarchy plugin, it runs unsandboxed inside the
user's shell process; install it only from a source you trust.

## License

[MIT](LICENSE)
