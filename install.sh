#!/bin/bash
# install.sh - set up frame-stream from a fresh Mac, step by step. Run it from this repository, in Terminal:
#
#   cd ~/src/frame-stream && ./install.sh
#
# Each step checks first, says what it found, and asks before it installs or changes anything. Run it again at any
# time: finished steps are skipped, so it is also the way to pick up after a step that stopped you.
#
#   1. Xcode and its Metal compiler (the runtime's shaders need them; the App Store has Xcode)
#   2. Homebrew, then cmake, ninja and vulkan-headers from it
#   3. the runtime: OXRSys cloned into ~/src/oxrsys, this repository's patches applied, built, tested, configured
#   4. Godot 4.7.2, the engine the test scene runs in, into /Applications (checksum checked)
#   5. the headset: an ssh name and key, then the headset side built there by frame/install.sh, and 90 Hz
#   6. optional: game sound (BlackHole 2ch and sox)
#   7. frame-stream doctor, which checks the whole setup and changes nothing
#
# Options: --mac (steps 1-4 and 6 only), --headset (step 5 only), --quest (steps 1-4 only, for a Meta Quest: QUEST.md),
# --yes (answer yes to every question; Homebrew and Xcode can still ask for your password), --check (report what is
# missing, change nothing).
# Bash 3.2 (the one macOS ships): no associative arrays, no ${x,,}.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${FRAME_STREAM_OXRSYS:-$HOME/src/oxrsys}
BASE=b41d892137f2e4ac71ccca6b2ea472e74968d8ce   # OXRSys main when the runtime patches were made
GODOT_VER=4.7.2
GODOT_ZIP=Godot_v${GODOT_VER}-stable_macos.universal.zip
GODOT_SHA=c58a24e31d720be9d62f60cb5627c4e695fb72f21b0cfe1bc9ccaa9a3b3ba63e   # GitHub's digest for that zip
mac=1; headset=1; quest=0; yes=0; check=0
for a in "$@"; do case $a in
  --mac) headset=0;; --headset) mac=0;; --quest) headset=0; quest=1;; --yes|-y) yes=1;; --check) check=1;;
  -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "install: unknown option $a (try --help)"; exit 2;; esac; done

b=$(tput bold 2>/dev/null || true); n=$(tput sgr0 2>/dev/null || true)
step(){ echo; echo "${b}== $*${n}"; }
say(){ echo "   $*"; }
ok(){ echo "   ok: $*"; }
todo=0
miss(){ echo "   MISSING: $*"; todo=$((todo+1)); }
die(){ echo; echo "${b}install stopped:${n} $*"; echo "Fix that, then run ./install.sh again; finished steps are skipped."; exit 1; }
ask(){   # ask "question" -> 0 for yes
  [ "$check" = 1 ] && return 1
  [ "$yes" = 1 ] && { echo "   $1 [y/N] y (--yes)"; return 0; }
  printf '   %s [y/N] ' "$1"; read -r r </dev/tty || return 1
  case $r in y|Y|yes|YES) return 0;; *) return 1;; esac
}

[ "$(uname -s)" = Darwin ] || die "this is the Mac installer; the headset side is frame/install.sh, which step 5 runs for you"
[ "$(uname -m)" = arm64 ] || die "frame-stream needs an Apple Silicon Mac (this one reports $(uname -m))"
[ "$(id -u)" != 0 ] || die "run it as yourself, not with sudo; it asks for your password when a step needs it"
[ -d "$HERE/patches/runtime" ] || die "run it from a full copy of this repository (no patches/runtime next to it)"
[ "$check" = 1 ] && echo "install: --check, so nothing is changed; each step says what it would do"

brew_env(){   # Homebrew is on the PATH of a login shell only; find it either way
  for p in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$p" ] && { eval "$("$p" shellenv)"; return 0; }; done
  return 1
}

if [ "$mac" = 1 ]; then
  # ---- 1. Xcode -------------------------------------------------------------------------------------------------
  step "1/7 Xcode and the Metal compiler"
  dev=$(xcode-select -p 2>/dev/null || true)
  if [ -z "$dev" ] || [ "${dev#/Library/Developer/CommandLineTools}" != "$dev" ]; then
    xc=$(ls -d /Applications/Xcode*.app 2>/dev/null | head -1)
    if [ -z "$xc" ]; then
      miss "Xcode (the full app, not just the command line tools: the runtime's Metal shaders need it)"
      say "Install it from the App Store (free, large; it takes a while), open it once and let it finish, then run"
      say "./install.sh again."
      ask "Open Xcode's App Store page now?" && open "macappstore://apps.apple.com/app/id497799835"
      [ "$check" = 1 ] || die "Xcode is not installed yet"
    else
      say "Xcode is at $xc but the developer tools point at ${dev:-nothing}; switching them needs your password"
      if ask "Run: sudo xcode-select -s $xc/Contents/Developer ?"; then
        sudo xcode-select -s "$xc/Contents/Developer" || die "xcode-select failed"
      else miss "developer tools pointed at Xcode"; fi
    fi
  else ok "Xcode at $dev"; fi
  if [ "$check" = 0 ] && ! xcodebuild -checkFirstLaunchStatus >/dev/null 2>&1; then
    say "Xcode has not finished its first launch (licence and components); this needs your password"
    if ask "Run: sudo xcodebuild -runFirstLaunch ?"; then sudo xcodebuild -license accept && sudo xcodebuild -runFirstLaunch || die "Xcode's first launch failed"; fi
  fi
  if xcrun -f metal >/dev/null 2>&1; then ok "Metal compiler"
  else
    miss "the Metal compiler (Xcode downloads it separately)"
    if ask "Run: xcodebuild -downloadComponent MetalToolchain ? (about 700 MB)"; then
      xcodebuild -downloadComponent MetalToolchain || die "the Metal toolchain download failed"
      xcrun -f metal >/dev/null 2>&1 && ok "Metal compiler" || die "still no Metal compiler after the download"
    fi
  fi

  # ---- 2. Homebrew ----------------------------------------------------------------------------------------------
  step "2/7 Homebrew, cmake, ninja, vulkan-headers"
  if brew_env; then ok "Homebrew at $(command -v brew)"
  else
    miss "Homebrew (https://brew.sh, a tool that installs other tools)"
    say "Its official installer asks for your password and may install Apple's command line tools first."
    if ask "Run Homebrew's installer now?"; then
      /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)" || die "Homebrew's installer failed"
      brew_env || die "Homebrew installed, but brew is not where it should be (/opt/homebrew)"
      if ! grep -qs 'brew shellenv' "$HOME/.zprofile"; then
        say "adding Homebrew to new Terminal windows (one line in ~/.zprofile, as Homebrew's installer suggests)"
        echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> "$HOME/.zprofile"
      fi
      ok "Homebrew"
    fi
  fi
  if command -v brew >/dev/null; then
    need=""; for f in cmake ninja vulkan-headers; do brew list --versions $f >/dev/null 2>&1 || need="$need $f"; done
    if [ -z "$need" ]; then ok "cmake, ninja, vulkan-headers"
    else
      miss "brew packages:$need"
      if ask "Run: brew install$need ?"; then brew install $need || die "brew install$need failed"; ok "brew packages"; fi
    fi
  fi

  # ---- 3. the runtime -------------------------------------------------------------------------------------------
  step "3/7 the runtime (OXRSys with this repository's patches) in $SRC"
  want=$(ls "$HERE"/patches/runtime/*.patch | wc -l | tr -d ' ')
  if [ -d "$SRC/.git" ]; then
    have=$(git -C "$SRC" rev-list --count $BASE..HEAD 2>/dev/null || echo "?")
    if [ "$have" = "$want" ] && git -C "$SRC" diff --quiet HEAD 2>/dev/null; then ok "source: $want patches applied"; src_ok=1
    else
      say "$SRC exists but is not this copy's $want patches on $BASE (it has: $have, or local changes)."
      say "Move it aside (mv $SRC $SRC.old) and run ./install.sh again to get a fresh one."
      miss "a matching runtime source"; src_ok=0
    fi
  else
    src_ok=0
    miss "the runtime source"
    if ask "Clone OXRSys from GitHub into $SRC and apply the $want patches?"; then
      mkdir -p "$(dirname "$SRC")"
      git clone https://github.com/demonixis/oxrsys "$SRC" || die "git clone failed (internet? see Requirements)"
      git -C "$SRC" checkout -q -b frame-stream $BASE || die "checkout of $BASE failed"
      # git am needs a name to commit under; a fresh Mac has none, so give it one for this repository only
      git -C "$SRC" -c user.name=frame-stream -c user.email=frame-stream@localhost am -q "$HERE"/patches/runtime/*.patch \
        || { git -C "$SRC" am --abort 2>/dev/null; die "a patch did not apply; this should not happen on $BASE, so please open an issue"; }
      ok "source: $want patches applied"; src_ok=1
    fi
  fi
  RT=$SRC/build/macos-arm64/runtime
  if [ "$src_ok" = 1 ]; then
    built=0; [ -f "$RT/liboxrsys-runtime.dylib" ] && [ "$RT/liboxrsys-runtime.dylib" -nt "$SRC/.git/HEAD" ] && built=1
    if [ "$built" = 1 ]; then ok "runtime built ($RT)"
    elif ask "Build the runtime and run its tests? (a few minutes; the first configure downloads four libraries)"; then
      TC=$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin
      ( cd "$SRC" && cmake --preset macos-arm64 -DCMAKE_C_COMPILER=$TC/clang -DCMAKE_CXX_COMPILER=$TC/clang++ \
          -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" -DCMAKE_PREFIX_PATH="$(brew --prefix vulkan-headers)" \
        && cmake --build build/macos-arm64 ) || die "the build failed; the lines above say where (docs/troubleshooting.md has the common ones)"
      ctest --test-dir "$SRC/build/macos-arm64" --output-on-failure | tail -3
      ctest --test-dir "$SRC/build/macos-arm64" >/dev/null 2>&1 || die "a runtime test failed (output above); please open an issue with it"
      ok "runtime built, all tests passed"
    else miss "the runtime build"; fi
    if [ -d "$RT" ]; then
      if cmp -s "$HERE/mac/oxrsys-runtime.toml" "$RT/oxrsys-runtime.toml"; then ok "runtime settings (mac/oxrsys-runtime.toml)"
      elif [ "$check" = 1 ]; then miss "runtime settings copied next to the runtime"
      else cp "$HERE/mac/oxrsys-runtime.toml" "$RT/" && ok "runtime settings copied (bitrate, pacing, foveation)"; fi
    fi
    U="$HOME/Library/Application Support/OXRSys/oxrsys-runtime.toml"
    [ -e "$U" ] && say "note: $U exists and the runtime reads it INSTEAD of the settings above; move it away unless you made it on purpose"
  fi

  # ---- 4. Godot -------------------------------------------------------------------------------------------------
  step "4/7 Godot $GODOT_VER (runs the test scene, engines/godot)"
  G=${FRAME_STREAM_GODOT:-/Applications/Godot.app/Contents/MacOS/Godot}
  if [ -x "$G" ]; then
    v=$("$G" --version 2>/dev/null | head -1)
    case $v in 4.7*) ok "Godot $v at $G";; *) say "Godot at $G reports '$v'; the test scene is made for 4.7. Set FRAME_STREAM_GODOT to a 4.7 if you keep both.";; esac
  else
    miss "Godot $GODOT_VER"
    if ask "Download Godot $GODOT_VER from GitHub (170 MB) into /Applications?"; then
      t=$(mktemp -d)
      curl -fL --progress-bar -o "$t/$GODOT_ZIP" "https://github.com/godotengine/godot/releases/download/${GODOT_VER}-stable/$GODOT_ZIP" || die "the Godot download failed"
      [ "$(shasum -a 256 "$t/$GODOT_ZIP" | cut -d' ' -f1)" = "$GODOT_SHA" ] || die "the Godot download's checksum is wrong; not installing it"
      ditto -x -k "$t/$GODOT_ZIP" "$t/x" && [ -d "$t/x/Godot.app" ] || die "could not unpack Godot"
      [ -e /Applications/Godot.app ] && die "/Applications/Godot.app exists but is not runnable; move it away first"
      mv "$t/x/Godot.app" /Applications/ && rm -rf "$t" && ok "Godot $GODOT_VER in /Applications"
    fi
  fi
fi

# ---- 5. the headset -------------------------------------------------------------------------------------------
if [ "$headset" = 1 ]; then
  step "5/7 the headset"
  say "First, by hand on the Frame: Settings > System > Enable Developer Mode, and set a long password when it asks."
  say "Its address is under Settings > Network. The Mac and the headset need to be on the same network for this step."
  if [ "$(ssh -G frame 2>/dev/null | awk '/^hostname /{print $2}')" != frame ]; then
    ok "ssh name 'frame' -> $(ssh -G frame | awk '/^hostname /{print $2}')"
  else
    miss "an ssh name for the headset"
    if [ "$check" = 0 ]; then
      printf '   The headset'"'"'s address (as shown under Settings > Network), or Return to skip the headset: '
      read -r addr </dev/tty || addr=""
      if [ -n "$addr" ]; then
        mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh"
        printf '\nHost frame\n  HostName %s\n  User steamos\n' "$addr" >> "$HOME/.ssh/config" && chmod 600 "$HOME/.ssh/config"
        ok "added Host frame ($addr) to ~/.ssh/config"
      fi
    fi
  fi
  if [ "$(ssh -G frame 2>/dev/null | awk '/^hostname /{print $2}')" != frame ]; then
    if ssh -o BatchMode=yes -o ConnectTimeout=8 frame true 2>/dev/null; then ok "ssh key works"
    elif [ "$check" = 1 ]; then miss "ssh key on the headset"
    else
      ls "$HOME"/.ssh/id_ed25519.pub >/dev/null 2>&1 || { say "making an ssh key for this Mac (no passphrase prompt: press Return twice if asked)"; ssh-keygen -t ed25519 -f "$HOME/.ssh/id_ed25519" -N "" -q; }
      say "copying the key to the headset: type the developer-mode password when asked (this is the only time)"
      ssh-copy-id -o ConnectTimeout=10 frame || die "could not log in to the headset (developer mode on? same network? the right address?)"
      ssh -o BatchMode=yes frame true || die "the key did not take; try: ssh frame"
      ok "ssh key works"
    fi
    if ssh -o BatchMode=yes -o ConnectTimeout=8 frame 'test -x ~/.local/bin/oxrsys-client' 2>/dev/null; then
      ok "headset side installed (run frame/install.sh there again after an update: ./install.sh --headset does it)"
      [ "$mac" = 0 ] && ask "Rebuild the headset side from this copy now?" && hs=1
    else miss "the headset side"; hs=0; ask "Build the headset side on the headset now? (it is kept slow on purpose so tracking never stutters; about 15-30 minutes)" && hs=1; fi
    if [ "${hs:-0}" = 1 ]; then
      ssh frame 'rm -rf frame-stream.new' && scp -rq "$HERE" frame:frame-stream.new && ssh frame 'rm -rf frame-stream && mv frame-stream.new frame-stream' \
        || die "copying this repository to the headset failed"
      ssh -t frame frame-stream/frame/install.sh || die "frame/install.sh failed on the headset (output above)"
      ssh frame '~/.local/bin/frame-vrsettings set steamvr defaultPerAppRefreshRate float 90' >/dev/null 2>&1 \
        && ok "headset apps run at 90 Hz" || say "set 90 Hz later, with SteamVR running: ssh frame '~/.local/bin/frame-vrsettings set steamvr defaultPerAppRefreshRate float 90'"
    fi
  else say "skipped the rest of the headset step: no address yet. Run ./install.sh --headset when you have it."; fi
fi

# A Quest has no use for steps 6 and 7 (its app plays no game sound; the doctor checks a Frame): quest/quest-test.sh
# takes over from here.
if [ "$quest" = 1 ]; then
  [ "$check" = 1 ] && { echo; echo "   $todo thing(s) missing."; exit $([ $todo = 0 ] && echo 0 || echo 1); }
  echo; echo "${b}Mac side ready.${n} quest/quest-test.sh goes on from here (QUEST.md)."; exit 0
fi

# ---- 6. sound -------------------------------------------------------------------------------------------------
if [ "$mac" = 1 ]; then
  step "6/7 game sound (optional)"
  if [ -d "/Library/Audio/Plug-Ins/HAL/BlackHole2ch.driver" ] && command -v sox >/dev/null; then ok "BlackHole 2ch and sox"
  elif command -v brew >/dev/null && ask "Install BlackHole 2ch and sox, so games' sound plays in the headset? (asks for your password)"; then
    brew install sox && brew install --cask blackhole-2ch && ok "BlackHole 2ch and sox" || say "sound setup failed; the stream works without it (FRAME_STREAM_AUDIO=0)"
  else say "skipped: the stream works without sound; run ./install.sh again to add it"; fi
fi

# ---- 7. doctor ------------------------------------------------------------------------------------------------
step "7/7 check"
if [ "$check" = 1 ]; then echo "   $todo thing(s) missing."; exit $([ $todo = 0 ] && echo 0 || echo 1); fi
if [ "$SRC" != "$HOME/src/oxrsys" ]; then   # frame-stream looks in ~/src/oxrsys unless told otherwise
  export FRAME_STREAM_RUNTIME="$SRC/build/macos-arm64/runtime"
  say "the runtime is in $SRC, not ~/src/oxrsys: put this line in ~/.zprofile so frame-stream finds it:"
  say "  export FRAME_STREAM_RUNTIME=$FRAME_STREAM_RUNTIME"
fi
"$HERE/bin/frame-stream" doctor || true
echo
echo "${b}Next:${n} connect the two (README, \"Which link\"), put the headset on, then:"
echo "   bin/frame-stream up --app godot      # and bin/frame-stream down when you are done"
echo "Unity and Unreal are set up from your own editor: engines/unity/README.md, engines/unreal/README.md."
