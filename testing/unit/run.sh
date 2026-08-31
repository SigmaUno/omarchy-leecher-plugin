#!/bin/sh
# Build and run the backend unit tests.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cc=${CC:-cc}
bin="$here/test_backend_logic"

echo "== unit: backend logic =="
$cc -std=c11 -Wall -Wextra -O2 -I"$here/../../backend" \
    "$here/test_backend_logic.c" \
    "$here/../../backend/library_handler.c" \
    "$here/../../backend/music_ripper.c" \
    "$here/../../backend/assembler.c" \
    "$here/../../backend/decoder.c" \
    "$here/../../backend/metadata.c" \
    $(pkg-config --cflags --libs sdl2 sndfile) -pthread -lm \
    -o "$bin"
"$bin"
rc=$?

echo
echo "== unit: widget helpers =="
if command -v node >/dev/null 2>&1; then
    node "$here/test_widget.mjs" || rc=1
else
    echo "  node not found -- skipping widget helper tests"
fi

exit $rc
