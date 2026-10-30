#!/bin/bash
# install.sh - build and install the Steam Frame client ON the headset. Copy this repository to the headset and run
# it there:
#
#   scp -r frame-stream steamos@<headset>:frame-stream
#   ssh steamos@<headset> frame-stream/frame/install.sh
#
# What it does, in order (each step says what it found):
#   1. source: clones https://github.com/demonixis/oxrsys into ~/src/oxrsys (OXRSYS_CLIENT_SRC to change it; the
#      choice is recorded in ~/games/oxrsys/client-src, so the launcher and uninstall.sh use the same one), checks
#      out upstream pull request #30 (the Steam Frame client) at the commit this series was made against, and applies
#      ../patches/client on a branch called frame-stream;
#   2. loader: copies the OpenXR loader from the Steam Linux Runtime 4 (ARM64) into ~/games/oxrsys/lib, because the
#      one SteamOS ships in /usr/lib is a stub with no symbols;
#   3. build: cmake + ninja, niced and with three jobs, because the headset's tracking shares the CPU with you;
#   4. install: ~/.local/bin/oxrsys-client (start/stop/status/log as a systemd user unit), frame-audio-rx and the launcher.
#
# frame/uninstall.sh takes all of it off again.
#
# It starts nothing. One setting changes: with SteamVR up and its per-app rate not 90 Hz, it sets 90 and prints the
# undo (uninstall.sh --settings puts it back). Run it again after a pull: it rebuilds from the patches in this copy.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); repo=$(dirname "$here")
GAME=$HOME/games/oxrsys
# The source dir: OXRSYS_CLIENT_SRC, else the one a previous install recorded, else ~/src/oxrsys. It is recorded in
# $GAME/client-src so the launcher runs the client built here and uninstall.sh finds it (both read the same order).
SRC=${OXRSYS_CLIENT_SRC:-$(cat "$GAME/client-src" 2>/dev/null || true)}; SRC=${SRC:-$HOME/src/oxrsys}
SLR=$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_4-arm64
BASE=5753dc1cd12494938683d3be3ee3d69bb0b8ead7   # demonixis/oxrsys pull/30/head when the series was made
NICE="nice -n 15 ionice -c3"

die(){ echo "install: $*" >&2; exit 1; }
say(){ echo "install: $*"; }
[ "$(uname -m)" = aarch64 ] || die "run this on the headset (this machine is $(uname -m))"
ls "$repo"/patches/client/*.patch >/dev/null 2>&1 || die "no patches in $repo/patches/client - copy the whole repository, not just frame/"
for t in git cmake ninja c++ pkg-config; do command -v $t >/dev/null || die "$t is missing (SteamOS on the Frame ships it; is this a SteamOS image?)"; done

# 1. source
if [ ! -d "$SRC/.git" ]; then
  say "cloning OXRSys into $SRC"
  mkdir -p "$(dirname "$SRC")"; git clone -q https://github.com/demonixis/oxrsys "$SRC"
fi
cd "$SRC"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "$SRC has local changes; commit or move them, or set OXRSYS_CLIENT_SRC to another directory"
if ! git cat-file -e "$BASE^{commit}" 2>/dev/null; then
  say "fetching pull request #30 (the Steam Frame client)"
  git fetch -q origin pull/30/head 2>/dev/null || true
  # The PR may since have moved on (a push or a rebase): GitHub still serves the commit itself by its full id.
  git cat-file -e "$BASE^{commit}" 2>/dev/null || git fetch -q origin "$BASE" 2>/dev/null || true
  git cat-file -e "$BASE^{commit}" 2>/dev/null || die "cannot fetch $BASE (pull request #30 when the patches were made) from GitHub. Open an issue."
fi
id=(); git config user.email >/dev/null || id=(-c user.name=frame-stream -c user.email=frame-stream@localhost)
git checkout -q -B frame-stream "$BASE"
git "${id[@]}" am -q "$repo"/patches/client/*.patch || { git am --abort; die "the client patches did not apply on $BASE"; }
say "source: $(git rev-list --count "$BASE"..HEAD) patches on pull request #30, at $(git rev-parse --short HEAD)"

# 2. loader: the newest runtime platform first (a stable path), then the copy Steam unpacks under var/tmp-* while it runs
lib=""
for d in $(ls -d "$SLR"/steamrt4_platform_*/files/lib/aarch64-linux-gnu 2>/dev/null | sort -rV) $(ls -d "$SLR"/var/tmp-*/usr/lib/aarch64-linux-gnu 2>/dev/null); do
  ls "$d"/libopenxr_loader.so.1* >/dev/null 2>&1 && { lib=$d; break; }
done
[ -n "$lib" ] || die "no libopenxr_loader.so.1 under $SLR - install 'Steam Linux Runtime 4.0 (arm64)' from the Steam library (it comes with any ARM64 game), then run this again"
mkdir -p "$GAME/lib"
cp -L "$(ls "$lib"/libopenxr_loader.so.1* | sort -V | tail -1)" "$GAME/lib/libopenxr_loader.so.1"
json=$(ls "$lib"/libjsoncpp.so.* 2>/dev/null | sort -V | tail -1 || true)
if [ -n "$json" ]; then cp -L "$json" "$GAME/lib/libjsoncpp.so.26"; else say "note: no libjsoncpp beside the loader; if the client fails to start, look for libjsoncpp.so.26 in the runtime"; fi
say "loader: $lib"

# 3. build
$NICE cmake -S clients/steam-frame -B build-frame -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenXR=ON -DOPENXR_LOADER_LIB="$GAME/lib/libopenxr_loader.so.1" >/dev/null
say "building (niced, three jobs; a fresh build takes a while)"
t0=$SECONDS
$NICE ninja -C build-frame -j3 steam_frame_client >/dev/null
say "built $SRC/build-frame/steam_frame_client in $((SECONDS-t0)) s"

# 4. install
mkdir -p "$HOME/.local/bin"
install -m 755 "$here/oxrsys-client" "$HOME/.local/bin/oxrsys-client"
install -m 755 "$here/launch-oxrsys-client" "$GAME/launch-oxrsys-client"
printf '%s\n' "$SRC" > "$GAME/client-src"   # the launcher runs $SRC/build-frame/steam_frame_client
install -m 755 "$here/frame-audio-rx" "$HOME/.local/bin/frame-audio-rx"   # game sound from the Mac (wine/README.md)
[ -f "$HOME/.config/openxr/1/active_runtime.json" ] || say "warning: no ~/.config/openxr/1/active_runtime.json; start SteamVR once so the loader can find it"

# 5. the refresh-rate tool (the Frame runs OpenXR apps at 72 Hz unless told otherwise)
ovr=/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers
if [ -f "$ovr/openvr.h" ]; then
  $NICE c++ -O2 -o "$HOME/.local/bin/frame-vrsettings" "$here/frame-vrsettings.cpp" -I"$ovr" \
    -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64
  rate=$(XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} timeout 10 "$HOME/.local/bin/frame-vrsettings" get steamvr defaultPerAppRefreshRate 2>/dev/null | awk '/^float/{print $2}' || true)
  say "frame-vrsettings installed; SteamVR runs apps at ${rate:-? (SteamVR is not up)} Hz"
  # 90 Hz is what this release is tuned for, and SteamVR gives apps 72 until told otherwise (2026-10-28: a fresh
  # install streamed at 72 and nobody said so). Set it when SteamVR is up; uninstall.sh --settings takes it out.
  if [ -n "$rate" ] && [ "${rate%.*}" != 90 ]; then
    if XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} timeout 10 "$HOME/.local/bin/frame-vrsettings" set steamvr defaultPerAppRefreshRate float 90 >/dev/null 2>&1; then
      say "SteamVR now runs apps at 90 Hz (was $rate; to undo: frame-vrsettings set steamvr defaultPerAppRefreshRate float $rate, or uninstall.sh --settings)"
    else say "could not set 90 Hz; with SteamVR up: frame-vrsettings set steamvr defaultPerAppRefreshRate float 90"; fi
  elif [ -z "$rate" ]; then say "for 90 Hz (what this release is tuned for), start SteamVR and run: frame-vrsettings set steamvr defaultPerAppRefreshRate float 90"; fi
else
  say "note: no OpenVR headers at $ovr, so frame-vrsettings was not built (see docs/troubleshooting.md, 72 Hz)"
fi
say "done. Start with: oxrsys-client start <mac-address>   (or frame-stream up from the Mac)"
