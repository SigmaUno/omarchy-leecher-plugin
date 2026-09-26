#!/bin/sh
set -eu

# Update an installed Leecher plugin to one exact, reviewed commit.
#
# Editing this checkout does not change the running plugin: the backend runs
# from a copy in $XDG_DATA_HOME/leecher-media, and the widget is a copy under
# $XDG_CONFIG_HOME/omarchy/plugins/<plugin id>/. This script closes that gap --
# it checks out the requested commit, rebuilds and reinstalls the backend
# (install-backend.sh), then redeploys the widget, which install-backend.sh
# does not do.
#
# Usage: sh update.sh --rev <40-character commit SHA> [--no-restart]
#        sh update.sh --no-pull [--no-restart]
#   --rev SHA     fetch that exact commit, verify it is on the remote's main,
#                 and deploy it from a detached checkout
#   --no-pull     deploy the working tree as-is, without touching git
#   --no-restart  do not restart the omarchy shell (the widget then keeps
#                 running the old QML until the shell is next restarted)
#
# There is deliberately no "update to the latest main": the installer builds
# and runs whatever it is handed, so a moving branch would execute code nobody
# pinned. Pick the commit you mean to run (e.g. the one a review approved).

do_pull=1
do_restart=1
rev=
want_rev=0
for argument in "$@"; do
    if [ "$want_rev" -eq 1 ]; then rev=$argument; want_rev=0; continue; fi
    case $argument in
        --rev) want_rev=1 ;;
        --rev=*) rev=${argument#--rev=} ;;
        --no-pull) do_pull=0 ;;
        --no-restart) do_restart=0 ;;
        -h|--help) sed -n '3,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$argument" >&2; exit 64 ;;
    esac
done
if [ "$want_rev" -eq 1 ]; then
    printf -- '--rev needs a commit SHA.\n' >&2
    exit 64
fi
if [ "$do_pull" -eq 0 ] && [ -n "$rev" ]; then
    printf -- '--rev and --no-pull are mutually exclusive.\n' >&2
    exit 64
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
config_home=${XDG_CONFIG_HOME:-"$HOME/.config"}
cd "$script_dir"

# ---- check out the pinned commit ------------------------------------------
if [ "$do_pull" -eq 1 ]; then
    if [ -z "$rev" ]; then
        printf 'Refusing to update without a pinned commit.\n' >&2
        printf 'Re-run with --rev <full 40-character SHA>, or --no-pull to deploy this checkout as-is.\n' >&2
        exit 64
    fi
    # Exactly 40 hex digits: an abbreviated SHA, a branch or a tag could each
    # resolve to something other than what was reviewed.
    rev=$(printf '%s' "$rev" | tr 'A-F' 'a-f')
    case $rev in
        *[!0-9a-f]*|'') printf 'Not a full commit SHA: %s\n' "$rev" >&2; exit 64 ;;
    esac
    if [ "${#rev}" -ne 40 ]; then
        printf 'Not a full 40-character commit SHA: %s\n' "$rev" >&2
        exit 64
    fi

    command -v git >/dev/null 2>&1 || { printf 'git is required to update.\n' >&2; exit 1; }
    git rev-parse --git-dir >/dev/null 2>&1 || {
        printf '%s is not a git checkout; use --no-pull.\n' "$script_dir" >&2
        exit 1
    }
    if [ -n "$(git status --porcelain)" ]; then
        printf 'Refusing to update: the checkout has uncommitted changes.\n' >&2
        printf 'Commit or stash them, or re-run with --no-pull.\n' >&2
        exit 1
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

    printf 'Fetching main from %s...\n' "$remote"
    git fetch --no-tags "$remote" "+refs/heads/main:refs/remotes/$remote/main"
    if ! git cat-file -e "$rev^{commit}" 2>/dev/null; then
        git fetch --no-tags "$remote" "$rev" 2>/dev/null || true
    fi
    if [ "$(git cat-file -t "$rev" 2>/dev/null)" != commit ]; then
        printf 'Commit %s was not found on %s.\n' "$rev" "$remote" >&2
        exit 1
    fi
    # A host can serve a commit by SHA that no branch of this repository holds
    # (GitHub does, for commits pushed to forks), so require it on main.
    if ! git merge-base --is-ancestor "$rev" "refs/remotes/$remote/main"; then
        printf 'Commit %s is not on %s/main; refusing to deploy it.\n' "$rev" "$remote" >&2
        exit 1
    fi

    printf 'Checking out %s (detached)...\n' "$rev"
    git -c advice.detachedHead=false checkout --quiet --detach "$rev"
    head=$(git rev-parse HEAD)
    if [ "$head" != "$rev" ] || [ -n "$(git status --porcelain)" ]; then
        printf 'Checkout did not land cleanly on %s (HEAD is %s).\n' "$rev" "$head" >&2
        exit 1
    fi
fi

# ---- backend --------------------------------------------------------------
printf '\n== backend ==\n'
sh "$script_dir/install-backend.sh"

# install-backend.sh uses `systemctl --user enable --now`, which does nothing to
# a service that is already running -- so a freshly installed binary would keep
# sitting on disk while the old process served the widget. Restart explicitly.
if command -v systemctl >/dev/null 2>&1; then
    printf 'Restarting the backend onto the new binary...\n'
    systemctl --user restart leecher-media.service || true
fi

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
if [ "$do_pull" -eq 1 ]; then
    printf 'Leecher is running commit %s.\n' "$rev"
else
    printf 'Leecher is running this working tree.\n'
fi
