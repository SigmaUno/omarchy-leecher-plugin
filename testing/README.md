# testing/

Test suite for the Leecher Omarchy plugin.

> **Branch note:** this directory lives on the `testing` branch only. It must
> **not** be merged into `main` — `main` ships the plugin, not its harness.
> When promoting work from `testing` to `main`, drop this directory
> (`git rm -r testing/`); `main`'s `.gitignore` keeps a stray copy untracked.

## Layout

| Path | What |
|---|---|
| `unit/test_backend_logic.c` | Fast, dependency-free unit tests for the pure logic in `backend/app.c` — time-agnostic helpers: shuffle/queue next-track selection, the play queue, JSON escaping, SSH-name validation, encoded-token parsing. Compiled by `#include`-ing `app.c` with its `main` renamed, so it tests the real code with no refactor. |
| `unit/test_widget_fmt.mjs` | Extracts the `fmt()` position formatter out of `BarWidget.qml` and checks it against known inputs (the `0:31`-for-a-5-minute-track regression). Needs `node`. |
| `integration/test_controls.sh` | Black-box: builds and spawns the headless backend against a generated fixture library, drives it through the control file, and asserts on `status.json` transitions for every control command (play/pause, seek, next/prev, autoplay, shuffle, repeat, volume, mute, queue, output). |
| `integration/make_fixture.sh` | Generates a short silent WAV + a `library.json` pointing at it, into a throwaway dir. |
| `run.sh` | Runs everything; non-zero exit on any failure. This is what CI invokes. |

## Running

```sh
testing/run.sh              # all suites
testing/unit/run.sh         # unit only
testing/integration/test_controls.sh   # integration only
```

Requirements: a C compiler, `pkg-config`, SDL2 + libsndfile dev files (same as
building the backend), `jq`, and `python3`. `node` is optional (the widget
`fmt` test is skipped without it).
