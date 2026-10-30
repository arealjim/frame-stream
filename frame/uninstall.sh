#!/bin/bash
# SPDX-License-Identifier: MIT
# uninstall.sh - take off the headset what install.sh put there. Run it on the headset:
#
#   ssh steamos@<headset> frame-stream/frame/uninstall.sh [--settings] [--source]
#
# Always: stops the client and audio units, removes ~/.local/bin/{oxrsys-client,frame-audio-rx,frame-vrsettings}
# and ~/games/oxrsys (the launcher and the copied OpenXR loader).
#   --settings  also removes SteamVR's defaultPerAppRefreshRate override (back to Valve's 72 Hz default for OpenXR
#               apps); needs SteamVR running and frame-vrsettings still installed, so it runs first
#   --source    also deletes the OXRSys clone install.sh made (~/src/oxrsys, or the OXRSYS_CLIENT_SRC install.sh recorded); without it the
#               clone and its build stay, and the script says where
# Nothing outside your home directory is touched, and nothing needs sudo.
set -euo pipefail
# The same order as install.sh and the launcher: OXRSYS_CLIENT_SRC, else the directory install.sh recorded, else ~/src/oxrsys.
SRC=${OXRSYS_CLIENT_SRC:-$(cat "$HOME/games/oxrsys/client-src" 2>/dev/null || true)}; SRC=${SRC:-$HOME/src/oxrsys}
settings=0 source=0
for a in "$@"; do case "$a" in --settings) settings=1;; --source) source=1;; *) echo "usage: uninstall.sh [--settings] [--source]" >&2; exit 2;; esac; done
say(){ echo "uninstall: $*"; }
[ "$(uname -m)" = aarch64 ] || { echo "uninstall: run this on the headset" >&2; exit 1; }

if [ $settings = 1 ]; then
  if [ -x "$HOME/.local/bin/frame-vrsettings" ]; then
    XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} timeout 10 "$HOME/.local/bin/frame-vrsettings" remove steamvr defaultPerAppRefreshRate \
      || say "could not reset defaultPerAppRefreshRate (is SteamVR running?); everything else continues"
  else
    say "frame-vrsettings is not installed, so the refresh-rate setting was left as it is"
  fi
fi
for u in oxrsys-client frame-audio-rx; do
  if systemctl --user is-active --quiet "$u" 2>/dev/null; then systemctl --user stop "$u"; say "stopped $u"; fi
done
for f in oxrsys-client frame-audio-rx frame-vrsettings; do
  if [ -e "$HOME/.local/bin/$f" ]; then rm -f "$HOME/.local/bin/$f"; say "removed ~/.local/bin/$f"; fi
done
if [ -d "$HOME/games/oxrsys" ]; then rm -rf "$HOME/games/oxrsys"; say "removed ~/games/oxrsys"; fi
if [ $source = 1 ]; then
  case "$SRC" in ""|/|"$HOME"|"$HOME/") echo "uninstall: refusing to delete '$SRC'" >&2; exit 2;; esac
  if [ -d "$SRC/.git" ]; then rm -rf "$SRC"; say "removed $SRC"; fi
elif [ -d "$SRC" ]; then
  say "kept the OXRSys clone and build at $SRC (--source deletes it)"
fi
say "done"
