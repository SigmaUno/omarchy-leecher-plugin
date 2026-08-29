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

printf '%s\n' 'Building Leecher backend...'
make -C "$script_dir/backend" app library-handler

printf 'Installing backend to %s\n' "$install_dir"
install -d "$install_dir" "$unit_dir"
install -m 0755 "$script_dir/backend/app" "$install_dir/app"
install -m 0755 "$script_dir/backend/library-handler" "$install_dir/library-handler"
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
