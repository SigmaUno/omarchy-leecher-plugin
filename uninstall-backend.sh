#!/bin/sh
set -eu

# Stops and removes only the service and installed binaries. The music library
# is intentionally preserved at $LEECHER_MEDIA_DIR/library/ for safety.

data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
install_dir=${LEECHER_MEDIA_DIR:-"$data_home/leecher-media"}
unit_path=$config_home/systemd/user/leecher-media.service

if command -v systemctl >/dev/null 2>&1; then
    systemctl --user disable --now leecher-media.service 2>/dev/null || true
    systemctl --user daemon-reload
fi
rm -f "$unit_path" "$install_dir/app" "$install_dir/library-handler"

if [ -d "$install_dir" ]; then
    printf 'Preserved your library and data in %s\n' "$install_dir"
fi

printf '%s\n' 'Leecher backend service removed.'
