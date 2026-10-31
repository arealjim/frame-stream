#!/bin/bash
# quest-test.sh - try frame-stream with a Meta Quest 3 or 3S, and compare it with stock OXRSys. QUEST.md explains it.
# Run it from this repository, in Terminal:
#
#   cd ~/src/frame-stream && quest/quest-test.sh
#
# What it does, asking before each change:
#   1. the Mac side: ./install.sh --quest (Xcode, Homebrew, the patched runtime, Godot)
#   2. stock OXRSys, unpatched, at the same commit, built next to it in ~/src/oxrsys-stock (the comparison)
#   3. adb (Android's USB tool, from Homebrew) and the Quest, connected by its USB-C cable
#   4. the Quest app: OXRSys's own headset app, prebuilt from the same commit (checksum checked), installed over USB
#   5. two 5-minute runs of the Godot test scene, stock first, then frame-stream, with a few questions after each
#   6. a zip of the logs and your answers on the Desktop, to send back
#
# Run it again at any time: finished steps are skipped, and it asks before repeating a run you have done.
# Options: --check (report what is missing, change nothing), --pack (only make the zip again),
#          --only stock|frame-stream (one run, e.g. to repeat it), --minutes N (run length, default 5).
# Settings: QUEST_APK=/path/to/app.apk installs your own build of clients/android-vr instead of the download.
# Bash 3.2 (the one macOS ships): no associative arrays, no ${x,,}.
set -uo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
BASE=b41d892137f2e4ac71ccca6b2ea472e74968d8ce   # OXRSys main when the runtime patches were made (install.sh's BASE)
FS_SRC=${FRAME_STREAM_OXRSYS:-$HOME/src/oxrsys}
STOCK_SRC=${QUEST_STOCK_OXRSYS:-$HOME/src/oxrsys-stock}
APK_URL=https://github.com/arealjim/frame-stream/releases/download/quest-client-b41d892/oxrsys-quest-b41d892.apk
APK_SHA=2938511aa8d3fb6e847e056df5ca9e13f7e516e5e38265004a1ef0828eecbf38   # oxrsys-quest-b41d892.apk, versionName 1.3.0
PKG=net.demonixis.oxrsys.android
ACT=$PKG/com.oculus.NativeActivity
APPDATA="$HOME/Library/Application Support/OXRSys"
OUT="$HOME/Desktop/frame-stream-quest-results"
GODOT=${FRAME_STREAM_GODOT:-/Applications/Godot.app/Contents/MacOS/Godot}
minutes=5; check=0; pack_only=0; only=""
while [ $# -gt 0 ]; do case $1 in
  --check) check=1;; --pack) pack_only=1;;
  --only) only=${2:-}; shift; case $only in stock|frame-stream) ;; *) echo "quest-test: --only stock or --only frame-stream"; exit 2;; esac;;
  --minutes) minutes=${2:-}; shift; case $minutes in ''|*[!0-9]*) echo "quest-test: --minutes takes a whole number"; exit 2;; esac;;
  -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "quest-test: unknown option $1 (try --help)"; exit 2;; esac; shift; done

b=$(tput bold 2>/dev/null || true); n=$(tput sgr0 2>/dev/null || true)
step(){ echo; echo "${b}== $*${n}"; }
say(){ echo "   $*"; }
ok(){ echo "   ok: $*"; }
todo=0
miss(){ echo "   MISSING: $*"; todo=$((todo+1)); }
die(){ echo; echo "${b}quest-test stopped:${n} $*"; echo "Fix that, then run quest/quest-test.sh again; finished steps are skipped."; exit 1; }
ask(){   # ask "question" -> 0 for yes
  [ "$check" = 1 ] && return 1
  printf '   %s [y/N] ' "$1"; read -r r </dev/tty || return 1
  case $r in y|Y|yes|YES) return 0;; *) return 1;; esac
}
pause(){ printf '   %s ' "${1:-Press Return to go on.}"; read -r _ </dev/tty || true; }
brew_env(){ for p in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$p" ] && { eval "$("$p" shellenv)"; return 0; }; done; return 1; }

[ "$(uname -s)" = Darwin ] || die "this runs on the Mac that streams to the Quest"
[ "$(uname -m)" = arm64 ] || die "frame-stream needs an Apple Silicon Mac (this one reports $(uname -m))"
[ "$(id -u)" != 0 ] || die "run it as yourself, not with sudo"
[ -x "$HERE/install.sh" ] && [ -d "$HERE/patches/runtime" ] || die "run it from a full copy of this repository"
brew_env || true

# ---- results ---------------------------------------------------------------------------------------------------
pack(){
  [ -d "$OUT" ] || die "no results yet in $OUT"
  {
    echo "date: $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "frame-stream copy: $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "mac: $(sysctl -n hw.model) / $(sysctl -n machdep.cpu.brand_string) / $(( $(sysctl -n hw.memsize) / 1073741824 )) GB"
    echo "macos: $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
    if command -v adb >/dev/null && [ "$(adb get-state 2>/dev/null)" = device ]; then
      echo "headset: $(adb shell -n getprop ro.product.model | tr -d '\r') / OS $(adb shell -n getprop ro.build.version.incremental | tr -d '\r')"
      echo "quest app: $(adb shell -n dumpsys package $PKG 2>/dev/null | awk -F= '/versionName/{print $2; exit}' | tr -d '\r')"
    fi
  } > "$OUT/system.txt"
  local z="$HOME/Desktop/frame-stream-quest-results-$(date +%Y%m%d-%H%M).zip"
  ( cd "$(dirname "$OUT")" && zip -qr "$z" "$(basename "$OUT")" ) || die "could not make the zip"
  echo; echo "${b}Done.${n} Your results: $z"
  say "It holds the runtime's logs, the Quest app's own log lines, the Godot log, your answers and system.txt (Mac"
  say "model, macOS version, headset model). Look inside first if you like; then attach it to an issue at"
  say "https://github.com/arealjim/frame-stream/issues (or send it to whoever gave you QUEST.md)."
}
[ "$pack_only" = 1 ] && { pack; exit 0; }
[ "$check" = 1 ] && echo "quest-test: --check, so nothing is changed; each step says what it would do"

# ---- 1. Mac side -----------------------------------------------------------------------------------------------
step "1/6 the Mac side (install.sh --quest)"
if [ "$check" = 1 ]; then "$HERE/install.sh" --quest --check || todo=$((todo+1))
else "$HERE/install.sh" --quest || die "install.sh stopped (above); run it again after fixing that"; brew_env || true; fi
FS_RT=$FS_SRC/build/macos-arm64/runtime
[ -f "$FS_RT/liboxrsys-runtime.dylib" ] || [ "$check" = 1 ] || die "no patched runtime at $FS_RT after install.sh"

# ---- 2. stock OXRSys -------------------------------------------------------------------------------------------
step "2/6 stock OXRSys (no patches, the comparison) in $STOCK_SRC"
ST_RT=$STOCK_SRC/build/macos-arm64/runtime
if [ -d "$STOCK_SRC/.git" ]; then
  if [ "$(git -C "$STOCK_SRC" rev-parse HEAD 2>/dev/null)" = $BASE ] && git -C "$STOCK_SRC" diff --quiet HEAD; then ok "source at $BASE"
  else die "$STOCK_SRC is not a clean OXRSys at $BASE; move it aside (mv $STOCK_SRC $STOCK_SRC.old) and run again"; fi
else
  miss "the stock source"
  if ask "Clone OXRSys again into $STOCK_SRC, unpatched, at the same commit?"; then
    git clone -q https://github.com/demonixis/oxrsys "$STOCK_SRC" && git -C "$STOCK_SRC" checkout -q --detach $BASE \
      || die "git clone failed (internet?)"
    ok "source at $BASE"
  fi
fi
if [ -d "$STOCK_SRC/.git" ]; then
  if [ -f "$ST_RT/liboxrsys-runtime.dylib" ]; then ok "stock runtime built"
  elif ask "Build the stock runtime? (a few minutes)"; then
    TC=$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin
    ( cd "$STOCK_SRC" && cmake --preset macos-arm64 -DCMAKE_C_COMPILER=$TC/clang -DCMAKE_CXX_COMPILER=$TC/clang++ \
        -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" -DCMAKE_PREFIX_PATH="$(brew --prefix vulkan-headers)" \
      && cmake --build build/macos-arm64 ) || die "the stock build failed (the lines above say where)"
    ok "stock runtime built"
  else miss "the stock runtime build"; fi
fi

# ---- 3. adb and the Quest --------------------------------------------------------------------------------------
step "3/6 adb and the Quest"
if command -v adb >/dev/null; then ok "adb at $(command -v adb)"
else
  miss "adb (Google's Android platform tools)"
  if command -v brew >/dev/null && ask "Run: brew install --cask android-platform-tools ?"; then
    brew install --cask android-platform-tools || die "the adb install failed"; ok "adb"
  fi
fi
# The first run of a newly downloaded adb waits for macOS to confirm it (Gatekeeper), with a window that can open
# behind Terminal; until someone answers it, every adb command hangs (seen 2026-10-31).
command -v adb >/dev/null && say "if nothing happens after this line: macOS is asking whether to open adb, which came from the internet." \
  && say "Look for that window (it may be behind Terminal) and choose Open."
quest_ready(){
  command -v adb >/dev/null || return 1
  local st; st=$(adb devices 2>/dev/null | awk 'NR>1 && NF>=2 {print $2}')
  [ "$(echo "$st" | grep -c .)" -gt 1 ] && die "more than one Android device is connected to adb; unplug all but the Quest"
  case $st in
    device) return 0;;
    "") say "no headset on adb yet";;
    unauthorized) say "the headset sees this Mac but has not allowed it yet: put it on and accept \"Allow USB debugging\"";;
    *) say "adb reports: $st";;
  esac; return 1
}
if [ "$check" = 1 ]; then quest_ready && ok "Quest connected" || miss "a connected Quest"
elif command -v adb >/dev/null; then
  until quest_ready; do
    say "On the Quest: developer mode on (QUEST.md, step 1), then the USB-C cable from the Quest to this Mac."
    say "Put the headset on: it asks once whether to allow USB debugging from this Mac. Choose Always allow."
    pause "Press Return to look again (or Ctrl-C to stop)."
  done
  ok "Quest connected: $(adb shell -n getprop ro.product.model | tr -d '\r')"
fi

# ---- 4. the Quest app ------------------------------------------------------------------------------------------
step "4/6 the Quest app (OXRSys's own headset app, built from $BASE)"
CACHE="$HOME/Library/Caches/frame-stream"; mkdir -p "$CACHE"
APK=${QUEST_APK:-$CACHE/oxrsys-quest-b41d892.apk}
if [ -z "${QUEST_APK:-}" ]; then
  if [ -f "$APK" ] && [ "$(shasum -a 256 "$APK" | cut -d' ' -f1)" = "$APK_SHA" ]; then ok "app downloaded"
  elif [ "$check" = 1 ]; then miss "the Quest app download"
  else
    say "downloading it from this repository's GitHub release (about 3 MB)"
    curl -fL --progress-bar -o "$APK.part" "$APK_URL" || die "the download failed (internet?)"
    [ "$(shasum -a 256 "$APK.part" | cut -d' ' -f1)" = "$APK_SHA" ] || { rm -f "$APK.part"; die "the download's checksum is wrong; not using it"; }
    mv "$APK.part" "$APK"; ok "app downloaded, checksum matches"
  fi
else [ -f "$APK" ] && ok "using your own app: $APK" || die "QUEST_APK=$APK does not exist"; fi
if [ "$check" = 0 ] && [ -f "$APK" ]; then
  if adb shell -n pm path $PKG 2>/dev/null | grep -q package: && [ -f "$CACHE/installed.sha" ] \
     && [ "$(cat "$CACHE/installed.sha")" = "$(shasum -a 256 "$APK" | cut -d' ' -f1)" ]; then ok "app installed on the Quest"
  else
    say "installing the app on the Quest"
    r=$(adb install -r "$APK" 2>&1); echo "$r" | tail -1 | sed 's/^/   /'
    if echo "$r" | grep -q INSTALL_FAILED_UPDATE_INCOMPATIBLE; then
      say "another build of OXRSys's Quest app is installed (from the store or your own build); it has to go first"
      ask "Uninstall it from the Quest and install this one?" || die "kept the other app; this test needs the matching one"
      adb uninstall $PKG >/dev/null; r=$(adb install "$APK" 2>&1); echo "$r" | tail -1 | sed 's/^/   /'
    fi
    echo "$r" | grep -q Success || die "the install failed (the line above says why)"
    shasum -a 256 "$APK" | cut -d' ' -f1 > "$CACHE/installed.sha"; ok "app installed"
  fi
fi

if [ "$check" = 1 ]; then echo; echo "   $todo thing(s) missing."; exit $([ $todo = 0 ] && echo 0 || echo 1); fi
[ -x "$GODOT" ] || die "no Godot at $GODOT (install.sh step 4)"

# ---- settings for a Quest --------------------------------------------------------------------------------------
# The runtime reads ~/Library/Application Support/OXRSys/oxrsys-runtime.toml if it exists, INSTEAD of the one beside
# it, and both runs need their own, so that file has to be out of the way.
U="$APPDATA/oxrsys-runtime.toml"
if [ -e "$U" ]; then
  say "$U exists and would override both runs' settings"
  ask "Rename it to oxrsys-runtime.toml.before-quest-test (rename it back afterwards to restore it)?" \
    && mv "$U" "$U.before-quest-test" || die "left it; both runs would use it"
fi
write_toml(){   # write_toml stock|frame-stream: the settings each run uses, beside its runtime
  if [ "$1" = stock ]; then
    # stock as shipped, at 90 Hz like frame-stream (stock ships 72)
    sed 's/^refresh_rate_hz = .*/refresh_rate_hz = 90/' "$STOCK_SRC/runtime/oxrsys-runtime.toml" > "$ST_RT/oxrsys-runtime.toml"
  else
    # frame-stream's settings, with the four Frame-tuned keys set for a Quest (README, "Other headsets"):
    # render size for a Quest 3, no position extrapolation (the Quest app does not do its own), a keyframe every 2 s;
    # pacing_lock_on stays.
    sed -e 's/^render_device = .*/render_device = "quest3"/' -e '/^pose_prediction_horizon_ms = /d' \
        -e 's/^keyframe_interval_sec = .*/keyframe_interval_sec = 2/' "$HERE/mac/oxrsys-runtime.toml" > "$FS_RT/oxrsys-runtime.toml"
  fi
}

# ---- 5. the runs -----------------------------------------------------------------------------------------------
GPID=""; LPID=""
stop_run(){
  [ -n "$GPID" ] && kill "$GPID" 2>/dev/null; GPID=""
  adb shell -n am force-stop $PKG >/dev/null 2>&1 || true
  [ -n "$LPID" ] && kill "$LPID" 2>/dev/null; LPID=""
}
trap 'echo; stop_run; exit 130' INT TERM
answer(){   # answer "question" file
  printf '   %s\n   > ' "$1"; local a; read -r a </dev/tty || a=""
  printf '%s\n  %s\n' "$1" "${a:-(no answer)}" >> "$2"
}
run_one(){   # run_one stock|frame-stream
  local name=$1 rt d; [ "$name" = stock ] && rt=$ST_RT || rt=$FS_RT
  d="$OUT/$name"
  if [ -f "$d/answers.txt" ]; then ask "The $name run is done already. Run it again (its old results are replaced)?" || return 0; fi
  rm -rf "$d"; mkdir -p "$d"; write_toml "$name"
  echo; echo "${b}-- the $name run --${n}"
  [ "$name" = stock ] && say "Stock OXRSys, as its author ships it (at 90 Hz). This is the comparison." \
                      || say "frame-stream: the same app and scene, with our patched runtime."
  pause "Headset off, cable in. Press Return to start it."
  mkdir -p "$APPDATA"; for f in oxrsys-runtime.log oxrsys-headset.log; do [ -f "$APPDATA/$f" ] && mv "$APPDATA/$f" "$APPDATA/$f.prev"; done
  for p in 9944 9945 9946 9948; do adb reverse tcp:$p tcp:$p >/dev/null || die "adb reverse failed (cable out?)"; done
  # The headset's whole log streams to a scratch file during the run (its buffer is too small to read afterwards);
  # only the app's own lines and the system's frame-rate lines (VrApi) are kept from it.
  adb logcat -c 2>/dev/null || true
  adb logcat -v threadtime > "$d/.logcat" 2>/dev/null </dev/null & LPID=$!
  adb shell -n am force-stop $PKG >/dev/null 2>&1; adb shell -n am start -n $ACT >/dev/null 2>&1 || die "could not start the app on the Quest"
  # The same launch as frame-stream's (bin/frame-stream, launch_godot), with this run's runtime.
  ( cd ~ || exit 1; export XR_RUNTIME_JSON="$rt/oxrsys-runtime.json"
    exec "$GODOT" --path "$HERE/engines/godot" --rendering-driver vulkan --rendering-method forward_plus --verbose \
      -ApplePersistenceIgnoreState YES ) > "$d/godot.log" 2>&1 < /dev/null &
  GPID=$!
  say "waiting for the Quest to connect (put the headset on now if it is asleep; up to 90 s)"
  local i=0; until grep -q 'Client connected' "$APPDATA/oxrsys-runtime.log" 2>/dev/null; do
    i=$((i+1)); sleep 1
    kill -0 $GPID 2>/dev/null || { say "Godot quit early (its log is in the results)"; break; }
    [ $i -ge 90 ] && { say "no connection after 90 s; ending this run and keeping its logs (they are the useful part)"; break; }
  done
  if grep -q 'Client connected' "$APPDATA/oxrsys-runtime.log" 2>/dev/null; then
    grep -m1 'Client connected' "$APPDATA/oxrsys-runtime.log" | sed 's/^.*Client connected/   connected:/'
    echo; say "${b}Put the headset on now and use it for $minutes minutes:${n} look around, turn your head quickly a few times,"
    say "step sideways, lean towards the cube and the panels, point and click with the controllers. Then look at"
    say "something and keep still for a while. The Mac says \"run finished\" out loud at the end. Return ends it early."
    read -r -t $((minutes*60)) _ </dev/tty || true
    /usr/bin/say "run finished" 2>/dev/null &
  fi
  local pid; pid=$(adb shell -n pidof $PKG 2>/dev/null | tr -d '\r')
  stop_run; sleep 2
  awk -v p="${pid:-none}" '$3==p || tolower($0) ~ /oxrsys/' "$d/.logcat" > "$d/quest-app.log"   # its own lines, and a crash's backtrace
  awk '$6 ~ /^VrApi/' "$d/.logcat" > "$d/quest-framerate.log"; rm -f "$d/.logcat"
  for f in oxrsys-runtime.log oxrsys-headset.log; do [ -f "$APPDATA/$f" ] && cp "$APPDATA/$f" "$d/"; done
  cp "${rt}/oxrsys-runtime.toml" "$d/settings.toml"
  echo; say "A few questions about the $name run (Return skips one):"
  local a="$d/answers.txt"; : > "$a"
  answer "Did a picture appear in the headset? (yes / no / for a while)" "$a"
  answer "How smooth was it, 1 (bad) to 5 (as smooth as a Quest game)?" "$a"
  answer "When you turned your head quickly or stepped sideways: judder, smearing, black edges, a lag? Describe it." "$a"
  answer "Any freezes? How long and how often?" "$a"
  answer "Controllers: did they show and follow your hands? Did the panel click?" "$a"
  answer "Anything else you noticed?" "$a"
  ok "$name run saved in $d"
}
step "5/6 the runs ($minutes minutes each)"
say "Before the first run: the Quest charged or on power, the cable in, the Guardian (play area) set up."
say "Results go into $OUT."
mkdir -p "$OUT"
case $only in
  stock) run_one stock;; frame-stream) run_one frame-stream;;
  *) run_one stock; run_one frame-stream
     if [ ! -f "$OUT/compare.txt" ]; then
       echo; answer "Comparing the two runs: which was better, and how? (stock / frame-stream / the same)" "$OUT/compare.txt"; fi;;
esac

# ---- 6. pack ---------------------------------------------------------------------------------------------------
step "6/6 the results"
write_toml frame-stream   # leave the patched runtime set for a Quest, so trying your own app next works
pack
