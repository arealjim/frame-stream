#!/bin/bash
# SPDX-License-Identifier: MIT
# run-game.sh <exe-path> [args...] - a Windows VR game under Wine + DXMT + wineopenxr -> OXRSys (x86_64) -> the Frame.
# Games check that Steam launched them, which they read from SteamAppId: it comes from STEAM_APPID, else the game's
# steam_appid.txt, else the appmanifest whose installdir holds the exe (a game under steamapps/common).
here=$(cd "$(dirname "$0")" && pwd)
export WINEPREFIX=${WINEPREFIX:-$here/prefix}
export WINEDEBUG=${WINEDEBUG:--all,+openxr,err+all}
export XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-$here/runtime/oxrsys-runtime.json}
export WINEDLLOVERRIDES="dxgi,d3d11,d3d10core=b;winemenubuilder.exe=d"
export MTL_HUD_ENABLED=${MTL_HUD_ENABLED:-0}
# shellcheck disable=SC1091  # pin-check.sh sits beside this script
. "$here/pin-check.sh"; pin_check "$here/wine" || exit 2
exe=${1:?usage: run-game.sh <exe-path> [args...]}; shift
[ -f "$exe" ] || { echo "run-game.sh: no such file: $exe" >&2; exit 2; }
dir=$(cd "$(dirname "$exe")" && pwd)
id=${STEAM_APPID:-}
d=$dir; while [ -z "$id" ] && [ "$d" != / ]; do
  [ -f "$d/steam_appid.txt" ] && id=$(tr -dc 0-9 < "$d/steam_appid.txt")
  d=$(dirname "$d")
done
if [ -z "$id" ] && [[ $dir == */steamapps/common/* ]]; then
  apps=${dir%%/steamapps/common/*}/steamapps; rest=${dir#*/steamapps/common/}; inst=${rest%%/*}
  m=$(grep -l -F "\"installdir\"		\"$inst\"" "$apps"/appmanifest_*.acf 2>/dev/null | head -1)
  [ -n "$m" ] && id=$(basename "$m" .acf | tr -dc 0-9)
fi
[ -n "$id" ] && export SteamAppId=$id SteamGameId=$id SteamOverlayGameId=$id \
  || echo "run-game.sh: no Steam app id found (set STEAM_APPID); a game that checks for Steam will quit" >&2
# Apple's Metal shader cache, which DXMT keeps in its per-exe dir (com.apple.metal), can make every later launch hang
# at 1 % CPU before the swapchain exists: Astroneer did, after a kill and after a clean quit alike, and with
# DXMT_SHADER_CACHE=0. Moving only com.apple.metal aside cured it at once; DXMT's own sqlite db is innocent. So start
# each launch without it (Metal recompiles; the first frame took ~20 s either way). DXMT_KEEP_METAL_CACHE=1 keeps it.
# The last copy stays as .stale for a post-mortem.
cache=${DXMT_CACHE_ROOT:-$(getconf DARWIN_USER_CACHE_DIR 2>/dev/null)dxmt}
m="$cache/$(basename "$exe")/com.apple.metal"
if [ "${DXMT_KEEP_METAL_CACHE:-0}" != 1 ] && [ -d "$m" ]; then
  rm -rf "$m.stale" && mv "$m" "$m.stale"
fi
cd "$dir" || exit 1
exec "$here/wine/bin/wine" "$(basename "$exe")" "$@"
