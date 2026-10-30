#!/bin/bash
# SPDX-License-Identifier: MIT
# run-steam.sh - the Windows Steam client inside the game prefix (a game's SteamAPI needs it running; log in once).
# Steam's browser draws black through DXMT, so Steam alone runs with D3D and Vulkan off; its webhelper is wrapped
# (steamwebhelper-wrapper/) to keep the GPU out of Chromium, and -noverifyfiles stops Steam restoring the original.
here=$(cd "$(dirname "$0")" && pwd)
export WINEPREFIX=${WINEPREFIX:-$here/prefix}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES="dxgi,d3d11,d3d10core,d3d12,vulkan-1,winevulkan=;winemenubuilder.exe=d"
# shellcheck disable=SC1091  # pin-check.sh sits beside this script
. "$here/pin-check.sh"; pin_check "$here/wine" || exit 2
cd "$WINEPREFIX/drive_c/Program Files (x86)/Steam" 2>/dev/null ||
  { echo "run-steam.sh: Steam is not installed in $WINEPREFIX (README step 6)" >&2; exit 2; }
exec "$here/wine/bin/wine" steam.exe -cef-disable-gpu -cef-disable-gpu-compositing -cef-in-process-gpu -cef-disable-sandbox -cef-disable-d3d11 -noverifyfiles "$@"
