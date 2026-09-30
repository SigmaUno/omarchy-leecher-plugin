#!/bin/sh
set -eu

# Build and install the backend used by the Leecher Omarchy widget. Everything
# stays in the current user's data and systemd-user directories; no sudo is
# required and the source checkout is never used as the service working tree.

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
install_dir=${LEECHER_MEDIA_DIR:-"$data_home/leecher-media"}
unit_dir=$config_home/systemd/user
unit_path=$unit_dir/leecher-media.service

require() {
    command -v "$1" >/dev/null 2>&1 || {
        printf 'Missing required command: %s\n' "$1" >&2
        exit 1
    }
}

for command in make cc pkg-config systemctl; do
    require "$command"
done

if ! pkg-config --exists sdl2 sndfile; then
    printf '%s\n' 'Missing build dependencies: SDL2 and libsndfile development files.' >&2
    printf '%s\n' 'On Omarchy: omarchy pkg add sdl2 libsndfile' >&2
    exit 1
fi

# Build in a private scratch copy, never in the checkout. backend/app and
# backend/library-handler are git-ignored, so a stale or planted binary there
# keeps `git status` clean and (with a newer mtime) makes make skip compiling
# it. The scratch copy holds source files only and is built with `make -B`, so
# every installed binary is compiled here from the sources that were checked.
if git -C "$script_dir" rev-parse --git-dir >/dev/null 2>&1; then
    stray=$(git -C "$script_dir" ls-files --others --ignored --exclude-standard -- backend \
        | grep -v -x -e backend/app -e backend/library-handler || true)
    if [ -n "$stray" ]; then
        printf 'Refusing to build: unexpected ignored files under backend/:\n%s\n' "$stray" >&2
        exit 1
    fi
fi
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/leecher-build.XXXXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
(cd "$script_dir/backend" && tar -cf - --exclude=./app --exclude=./library-handler .) \
    | tar -xf - -C "$build_dir"
rm -f "$build_dir/app" "$build_dir/library-handler"

printf '%s\n' 'Building Leecher backend...'
make -B -C "$build_dir" app library-handler

printf 'Installing backend to %s\n' "$install_dir"
install -d "$install_dir" "$unit_dir"
install -m 0755 "$build_dir/app" "$install_dir/app"
install -m 0755 "$build_dir/library-handler" "$install_dir/library-handler"
rm -rf "$build_dir"
trap - EXIT HUP INT TERM
if [ ! -f "$install_dir/library.json" ]; then
    install -m 0644 "$script_dir/backend/library.example.json" "$install_dir/library.json"
    printf 'Created an empty library at %s/library.json\n' "$install_dir"
fi

escaped_install_dir=$(printf '%s' "$install_dir" | sed 's/[\\&|]/\\&/g')
temporary_unit=$unit_path.tmp.$$
trap 'rm -f "$temporary_unit"' EXIT HUP INT TERM
sed "s|__LEECHER_DIR__|$escaped_install_dir|g" \
    "$script_dir/systemd/leecher-media.service" > "$temporary_unit"
install -m 0644 "$temporary_unit" "$unit_path"
rm -f "$temporary_unit"
trap - EXIT HUP INT TERM

systemctl --user daemon-reload
systemctl --user enable --now leecher-media.service

if command -v omarchy-shell >/dev/null 2>&1; then
    omarchy-shell shell rescanPlugins || true
fi

printf '%s\n' 'Leecher backend is running. Add or enable the plugin with Omarchy.'
