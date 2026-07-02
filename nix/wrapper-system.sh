#!/bin/sh
# focus-event-system-launch <username> <binary> [args...]
#
# Resolves the runtime uid of <username>, locates the user's niri IPC socket
# under /run/user/<uid>/, exports NIRI_SOCKET and XDG_RUNTIME_DIR, then execs
# the focus-event binary. The uid lookup happens at process start so the script
# works even when the user account is created after the systemd unit was
# generated (NixOS commonly assigns uids lazily).
#
# Why a wrapper rather than Environment= in the unit: the socket path includes
# a PID (`niri.wayland-<pid>.sock`) that is not known at nixos-rebuild time and
# may even rotate across niri restarts. Resolution must happen at unit start.

set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <username> <binary> [args...]" >&2
    exit 64
fi

USERNAME_ARG="$1"
shift

if ! UID_NUM="$(id -u "$USERNAME_ARG" 2>/dev/null)"; then
    echo "focus-event: user '$USERNAME_ARG' has no uid (not created yet?)" >&2
    exit 1
fi

RUNTIME_DIR="/run/user/$UID_NUM"
if [ ! -d "$RUNTIME_DIR" ]; then
    echo "focus-event: $RUNTIME_DIR does not exist (user not logged in?)" >&2
    exit 1
fi

# Pick the first matching socket. niri creates exactly one per running
# instance; the wildcard absorbs the PID-based suffix.
SOCKET="$(ls "$RUNTIME_DIR"/niri.wayland-*.sock 2>/dev/null | head -n1 || true)"
if [ -z "$SOCKET" ]; then
    echo "focus-event: no niri.wayland-*.sock under $RUNTIME_DIR (niri not running?)" >&2
    exit 1
fi

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export NIRI_SOCKET="$SOCKET"

exec "$@"
