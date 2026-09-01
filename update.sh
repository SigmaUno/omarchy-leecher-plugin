#!/bin/sh
set -eu

# Update an installed Leecher plugin to the latest main.
#
# Editing this checkout does not change the running plugin: the backend runs
# from a copy in $XDG_DATA_HOME/leecher-media, and the widget is a copy under
# $XDG_CONFIG_HOME/omarchy/plugins/<plugin id>/. This script closes that gap --
# it pulls main, rebuilds and reinstalls the backend (install-backend.sh), then
# redeploys the widget, which install-backend.sh does not do.
#
# Usage: sh update.sh [--no-pull] [--no-restart]
#   --no-pull     deploy the working tree as-is, without touching git
#   --no-restart  do not restart the omarchy shell (the widget then keeps
#                 running the old QML until the shell is next restarted)

do_pull=1
do_restart=1
for argument in "$@"; do
    case $argument in
        --no-pull) do_pull=0 ;;
        --no-restart) do_restart=0 ;;
        -h|--help) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$argument" >&2; exit 64 ;;
    esac
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
cd "$script_dir"

# ---- pull main ------------------------------------------------------------
if [ "$do_pull" -eq 1 ]; then
    command -v git >/dev/null 2>&1 || { printf 'git is required to pull.\n' >&2; exit 1; }
    git rev-parse --git-dir >/dev/null 2>&1 || {
        printf '%s is not a git checkout; use --no-pull.\n' "$script_dir" >&2
        exit 1
    }
    if [ -n "$(git status --porcelain)" ]; then
        printf 'Refusing to pull: the checkout has uncommitted changes.\n' >&2
        printf 'Commit or stash them, or re-run with --no-pull.\n' >&2
        exit 1
    fi

    branch=$(git rev-parse --abbrev-ref HEAD)
    if [ "$branch" != "main" ]; then
        printf 'Switching from %s to main (tree is clean, so nothing is lost).\n' "$branch"
        git checkout main
    fi

    # main may have no upstream configured, and the remote is not necessarily
    # called "origin", so resolve it rather than assuming either.
    if upstream=$(git rev-parse --abbrev-ref main@{upstream} 2>/dev/null); then
        remote=${upstream%%/*}
    else
        remote_count=$(git remote | wc -l)
        if [ "$remote_count" -eq 1 ]; then
            remote=$(git remote)
        else
            printf 'main has no upstream and there is not exactly one remote.\n' >&2
            printf 'Set one, e.g.: git branch --set-upstream-to=<remote>/main main\n' >&2
            exit 1
        fi
    fi

    printf 'Pulling main from %s...\n' "$remote"
    git pull --ff-only "$remote" main
fi

# ---- backend --------------------------------------------------------------
printf '\n== backend ==\n'
sh "$script_dir/install-backend.sh"

# ---- widget ---------------------------------------------------------------
# install-backend.sh only handles the backend; the widget is a plain file copy
# into the Omarchy plugin directory named after the manifest id.
plugin_id=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$script_dir/manifest.json")
if [ -z "$plugin_id" ]; then
    printf 'Could not read the plugin id from manifest.json\n' >&2
    exit 1
fi
plugin_dir=$config_home/omarchy/plugins/$plugin_id

printf '\n== widget ==\n'
printf 'Deploying BarWidget.qml to %s\n' "$plugin_dir"
install -d "$plugin_dir"
install -m 0644 "$script_dir/BarWidget.qml" "$plugin_dir/BarWidget.qml"
install -m 0644 "$script_dir/manifest.json" "$plugin_dir/manifest.json"

# ---- reload ---------------------------------------------------------------
if [ "$do_restart" -eq 1 ] && command -v omarchy >/dev/null 2>&1; then
    printf '\nRestarting the Omarchy shell so the widget reloads...\n'
    omarchy restart shell || printf 'Could not restart the shell; do it yourself to reload the widget.\n' >&2
elif [ "$do_restart" -eq 1 ]; then
    printf '\nomarchy not found; restart the shell yourself to reload the widget.\n'
fi

# ---- report ---------------------------------------------------------------
printf '\n== status ==\n'
if command -v systemctl >/dev/null 2>&1; then
    state=$(systemctl --user is-active leecher-media.service 2>/dev/null || true)
    restarts=$(systemctl --user show leecher-media.service -p NRestarts --value 2>/dev/null || echo '?')
    printf 'backend service: %s (restarts: %s)\n' "${state:-unknown}" "$restarts"
    if [ "$state" != "active" ]; then
        printf 'The backend is not running. Check:\n' >&2
        printf '  journalctl --user -u leecher-media.service -n 30\n' >&2
        exit 1
    fi
fi
printf 'Leecher is up to date.\n'
