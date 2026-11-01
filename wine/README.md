# PC VR games from the Mac, under free Wine (experimental)

**Experimental.** Three games work: Beat Saber, natively in VR, Half-Life: Alyx through OpenComposite, and Sifu through UEVR. The rest of this page is a recipe that worked on one
Mac; expect to debug.

Windows VR games, run on an Apple Silicon Mac under Wine and streamed into the Steam Frame. These are games you own, run
from your own Steam library. No Windows PC, no SteamVR, no CrossOver. Measured with Beat Saber (Unity, D3D11, native OpenXR): 90 Hz over the USB-C cable,
and at 90 Hz with sound in the headset over a home Wi-Fi access point.

```
Windows game (x86-64, D3D11)          OpenVR games: + OpenComposite's openvr_api.dll (OpenVR -> OpenXR)
  -> Wine 11.16 (Rosetta 2)           with our winemac patch, so DXMT runs on free Wine
  -> DXMT (D3D11 -> Metal)            shared textures with the runtime, no copy
  -> wineopenxr                       the Windows OpenXR loader's runtime, bridged to a macOS .so
  -> OXRSys runtime (x86-64)          our patches + the two below
  -> oxrsys-encoder-helper (arm64)    hardware HEVC; Rosetta processes get none
  -> USB-C or Wi-Fi -> the Frame client
game audio: Wine -> BlackHole 2ch -> frame-audio-tx -> UDP -> frame-audio-rx -> PipeWire on the headset
```

## Whose work this is

The chain from wineopenxr onwards is [oxrsys-winebridge](https://github.com/anoshmisiosdev/oxrsys-winebridge), which
streams Windows VR games from a Mac to a Meta Quest under CrossOver. Install its bridge by its own instructions; we
do not copy it. What this directory adds:

- **free Wine instead of CrossOver**: `winemac-dxmt_compat.c`. DXMT finds its Wine functions with `dlsym`, and only
  CrossOver's winemac exports them. This file adds the table to upstream winemac, in the layout DXMT expects, and
  creates the Metal view upstream only creates for OpenGL and Vulkan. Without it a game dies at "Failed to create
  metal view".
- **the Steam Frame** as the headset (the client is in `patches/client`).
- **a runtime patch** (`patches/runtime/0024`): shared swapchain textures for DXMT (taken from anoshmisiosdev's
  OXRSys fork, github.com/anoshmisiosdev/oxrsys, branch integration/main, commit 9f468bb, MPL-2.0). A second one,
  low-latency rate control in the arm64 encoder helper (0025), is left out: that encoder writes a full-range stream
  from the BGRA frames the helper gets, while the headset converts the picture as limited range.
- **sound into the headset**, and the launchers here.

## Install

**Versions are pinned, and the launchers check.** This recipe is Wine 11.16, DXMT `4ddb20e` and the bridge at
`88a97e9`, nothing newer. The winemac patch reads Wine's private window data, which Wine is still reshaping and does
not promise to keep, so on another Wine it fails when a game starts, not when it builds. `run-game.sh` and
`run-steam.sh` therefore refuse a Wine other than 11.16, or a winemac without the patch (`pin-check.sh`);
`FRAME_WINE_ANY=1` skips the check when you are trying a newer Wine on purpose. The lasting fix is upstream: Wine
merge requests !11058 and !11257 (gitlab.winehq.org) are two attempts at an official way for DXMT to get a Metal
layer from a window. Neither has landed; when one does, this patch can go.

You need everything in the top-level README for the runtime and the headset client first. Then, on the Mac:

Tools for all the steps below: Xcode (the full app, not only the Command Line Tools), Rosetta 2, and from Homebrew
`brew install bison mingw-w64 meson ninja cmake python sox`.

1. **Wine 11.16, exactly.** The winemac library in step 2 is built from 11.16's source and only fits 11.16, so take
   [Gcenx's 11.16 devel build](https://github.com/Gcenx/macOS_Wine_builds/releases/tag/11.16) itself, not the
   Homebrew `wine@devel` cask (it installs whatever is newest, into /Applications, and `brew upgrade` would remove
   the patch):

   ```sh
   curl -fLO https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.16/wine-devel-11.16-osx64.tar.xz
   shasum -a 256 wine-devel-11.16-osx64.tar.xz       # 6f9af818b7af6001aeed7818cb32bf0155598c5ea4e3b33380a03cf814e033cd
   tar -xf wine-devel-11.16-osx64.tar.xz            # -> Wine Devel.app
   xattr -dr com.apple.quarantine "Wine Devel.app"
   ln -s "$PWD/Wine Devel.app/Contents/Resources/wine" wine
   WINEPREFIX=$PWD/prefix wine/bin/wine wineboot -i  # the 64-bit prefix
   ```

2. **winemac with the DXMT table.** Get the matching Wine source, add the file, rebuild one library:

   ```sh
   curl -fLO https://dl.winehq.org/wine/source/11.x/wine-11.16.tar.xz
   shasum -a 256 wine-11.16.tar.xz                  # c66e2090343dcd727f7f7fd2f87ee0bfb0b118790c1d745ab7b8a4c3a4197f2f
   tar -xf wine-11.16.tar.xz && cd wine-11.16
   cp ../winemac-dxmt_compat.c dlls/winemac.drv/dxmt_compat.c
   perl -0pi -e 's/^SOURCES = \\\n/SOURCES = \\\n\tdxmt_compat.c \\\n/m' dlls/winemac.drv/Makefile.in   # perl, not sed: BSD sed varies on \n
   TC=$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin; SDK=$(xcrun --sdk macosx --show-sdk-path)
   export PATH=$(brew --prefix bison)/bin:$PATH        # macOS's own bison is too old for Wine
   arch -x86_64 ./configure --enable-archs=i386,x86_64 --disable-tests --without-x \
     --without-freetype --without-gstreamer --without-sdl --without-cups --without-sane --without-gphoto \
     --without-krb5 --without-netapi --without-pcap --without-usb --without-v4l2 --without-capi --without-inotify \
     CC="$TC/clang -arch x86_64 -isysroot $SDK" OBJC="$TC/clang -arch x86_64 -isysroot $SDK"
   arch -x86_64 make dlls/winemac.drv/winemac.so
   [ -f ../wine/lib/wine/x86_64-unix/winemac.so.upstream ] ||   # keep the first backup on a second run
     cp ../wine/lib/wine/x86_64-unix/winemac.so ../wine/lib/wine/x86_64-unix/winemac.so.upstream
   cp dlls/winemac.drv/winemac.so ../wine/lib/wine/x86_64-unix/winemac.so
   ```

   Xcode's compiler and SDK must be named: the Command Line Tools' SDK fails the configure. `arch -x86_64` makes
   Wine build for the Intel side that Rosetta runs; without it Wine configures for arm64 and the patch does not compile.

   **kernel32 with the guard-page fix** (for UEVR; on the stand's chain for every game since then). Stock
   Wine 11.16's `IsBadReadPtr` (and its siblings) catches an access violation but not `STATUS_GUARD_PAGE_VIOLATION`.
   The fault then reaches the game's unhandled-exception filter. Windows reports a guard page as a bad pointer and
   swallows the fault. UEVR probes the game's memory with these calls, and one probe killed Astroneer. `kernel32-isbadptr-guard-page.patch` adds that one line. Build it in the same source tree:

   ```sh
   patch -p1 < ../kernel32-isbadptr-guard-page.patch
   arch -x86_64 make BISON=$(brew --prefix bison)/bin/bison dlls/kernel32/x86_64-windows/kernel32.dll
   patch -R -p1 < ../kernel32-isbadptr-guard-page.patch          # keep the tree stock for the next build
   cp ../wine/lib/wine/x86_64-windows/kernel32.dll kernel32-stock.dll   # once, before the first install
   x86_64-w64-mingw32-strip -o kernel32.dll dlls/kernel32/x86_64-windows/kernel32.dll
   cp kernel32.dll ../wine/lib/wine/x86_64-windows/ && cp kernel32.dll ../prefix/drive_c/windows/system32/  # wineserver down
   ```

   Pass `BISON=` to make even with Homebrew's bison on PATH. A tree configured when macOS's bison 2.3 came first has
   that one baked in, and it fails on `tools/wrc/parser.y` ("syntax error, unexpected identifier"). Built on the Mac:
   unstripped 2,508,575 bytes, stripped 405,518 bytes, sha256
   `43a8f2d4c65d6887b12bb7646657fa5b273c8d83e409c02f430de3ad3cb8e6f8`. Gcenx's stock 11.16 kernel32 is 376,832
   bytes, sha256 starting `7faea9bc70292b3b`. The build is not byte-reproducible across toolchains, so check that
   the patch is in rather than comparing hashes.

3. **DXMT** at the commit the bridge pins (v0.80 is too old: its keyed mutex is missing). `build-dxmt.sh` builds it
   as DXMT's own CI does (LLVM 15 for x86-64, mingw, meson; 6 minutes from a fresh clone on an M5 MacBook Pro,
   mostly LLVM). DXMT's DLLs are Wine builtins and `run-game.sh` asks for builtins (`d3d11,dxgi,d3d10core=b`), so
   they replace Wine's own: copy the four DLLs (`d3d11`, `dxgi`, `d3d10core`, `winemetal`) into Wine's
   `lib/wine/x86_64-windows` **and** the prefix's `system32`, and `winemetal.so` into Wine's `lib/wine/x86_64-unix`.
   That is the install every result here was measured on.

4. **The bridge** (wineopenxr) from [oxrsys-winebridge](https://github.com/anoshmisiosdev/oxrsys-winebridge), built
   for x86-64 and installed into this Wine (11 s from a fresh clone, at the bridge's commit `88a97e9`):

   ```sh
   TC=$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin; SDK=$(xcrun --sdk macosx --show-sdk-path)
   X86="-DCMAKE_C_COMPILER=$TC/clang -DCMAKE_CXX_COMPILER=$TC/clang++ -DCMAKE_OBJC_COMPILER=$TC/clang \
        -DCMAKE_OBJCXX_COMPILER=$TC/clang++ -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release"
   git clone https://github.com/anoshmisiosdev/oxrsys-winebridge && cd oxrsys-winebridge
   git checkout 88a97e9 && git submodule update --init --recursive bridge && cd bridge
   cmake -B build $X86 && cmake --build build      # bash; in zsh write ${=X86}
   cp build/src/pe/wineopenxr.dll  ../../wine/lib/wine/x86_64-windows/
   cp build/src/unix/wineopenxr.so ../../wine/lib/wine/x86_64-unix/
   ```

   Then register it as the prefix's OpenXR runtime:

   ```sh
   mkdir -p prefix/drive_c/openxr
   printf '{"file_format_version":"1.0.0","runtime":{"name":"wineopenxr","library_path":"wineopenxr.dll"}}\n' \
     > prefix/drive_c/openxr/wineopenxr64.json
   WINEPREFIX=$PWD/prefix wine/bin/wine reg add 'HKLM\Software\Khronos\OpenXR\1' /v ActiveRuntime /t REG_SZ \
     /d 'C:\openxr\wineopenxr64.json' /f
   ```

   Name Xcode's compiler and SDK as above: the Command Line Tools' newer SDK cannot link x86-64 at all ("tapi
   error: unknown architecture").

5. **The runtime, built for x86-64**: OXRSys with the patches here (`patches/runtime`), configured with the same
   `$X86` flags, `cmake -G Ninja -B build/x64 $X86 && cmake --build build/x64 --target oxrsys_runtime` (33 s).
   Copy `build/x64/runtime/liboxrsys-runtime.dylib` into `runtime/` beside these scripts, with this repo's tuned
   `mac/oxrsys-runtime.toml` set to render at 1728 per eye for games (see "Settings that matter for games"), and an
   `oxrsys-runtime.json` that names the library:

   ```sh
   sed 's/^render_device = .*/render_device = "steam_frame"/' ../mac/oxrsys-runtime.toml > runtime/oxrsys-runtime.toml
   ```

   Optional: add `exit_app_after_client_gone_s = 20` under `[streaming]`, and a game quits by itself 20 s after you
   leave it from the headset (Quit in the SteamVR dashboard). Without it the game keeps running on the Mac.

   If `~/Library/Application Support/OXRSys/oxrsys-runtime.toml` exists, the runtime reads that one instead, for
   games and engines alike. Make the same change there, or move it aside.

   ```json
   {"file_format_version": "1.0.0", "runtime": {"name": "OXRSys Runtime", "library_path": "./liboxrsys-runtime.dylib"}}
   ```

   Then build the runtime once more natively (arm64, the top-level README) and copy its `oxrsys-encoder-helper` into
   `runtime/` too. A process under Rosetta gets no hardware HEVC encoder; the runtime finds the helper and hands it
   the frames.

6. **Windows Steam** in the prefix (Valve's `SteamSetup.exe /S`), started with `run-steam.sh`. Its browser draws
   black through DXMT, so Steam runs with D3D off, and its `steamwebhelper.exe` is replaced by the wrapper in
   `steamwebhelper-wrapper/` (`make` there; keep the original as `steamwebhelper_real.exe`). Log in. Clicks in
   Steam's window are not yet retested (Known problems), so start each download from the command line, which opens
   Steam's install dialog for it; that is how the games here were installed:

   ```sh
   WINEPREFIX=$PWD/prefix wine/bin/wine 'C:\Program Files (x86)\Steam\steam.exe' steam://install/546560   # the app id
   ```

   The wrapper goes in every `bin/cef/*/` folder (three here). Run the same loop again after a Steam client update:
   the update writes Valve's webhelper back over the wrapper, and the browser goes black again. The loop takes any
   Valve binary it finds (the wrapper is about 150 KB, Valve's several MB) as the new real one.

   ```sh
   for d in prefix/drive_c/'Program Files (x86)'/Steam/bin/cef/*/; do
     [ $(stat -f %z "$d/steamwebhelper.exe") -gt 1000000 ] && mv -f "$d/steamwebhelper.exe" "$d/steamwebhelper_real.exe"
     cp steamwebhelper-wrapper/steamwebhelper.exe "$d/"
   done
   ```

7. **Sound.** Install [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole) and `brew install sox`. Point
   Wine at BlackHole: in the prefix, `HKCU\Software\Wine\Drivers\winecoreaudio.drv` `DefaultOutput` must be the
   device's full endpoint id as Wine lists it under `HKLM\Software\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render`
   (for example `{0.0.0.00000000}.{323E261E-...}`). The device's name and its CoreAudio UID are both ignored.

## Run

```sh
./run-steam.sh &                                     # once per session; leave it running
./run-game.sh ".../steamapps/common/Beat Saber/Beat Saber.exe"
```

Then the headset client against the Mac (`frame-stream up --app external`, or `oxrsys-client start <mac-ip>` on the
headset), and the sound:

```sh
# on the Mac, from Terminal (macOS's microphone permission belongs to Terminal; from bare ssh it records silence)
mac/frame-audio-tx                       # USB-C: sends to the headset's tether address
mac/frame-audio-tx <headset-ip>          # Wi-Fi
# on the headset
frame-audio-rx start                     # USB-C
frame-audio-rx start <mac-ip>            # Wi-Fi: it says hello first, so the headset's firewall lets the reply in
```

Pass the game the arguments Steam would. Half-Life: Alyx without `-vr` starts flat and crashes in its renderer; Steam
launches it as `hlvr.exe -vr -steam -noasserts -nopassiveasserts +map startup`. `strings appcache/appinfo.vdf | grep -A3
<exe name>` in the Steam folder shows a game's.

`run-game.sh` sets the Steam app id a game checks for: from `STEAM_APPID`, else the game's `steam_appid.txt`, else
the Steam manifest that installed it.

**OpenVR games** (most SteamVR titles) talk OpenVR, not OpenXR. [OpenComposite](https://gitlab.com/znixian/OpenOVR)'s
64-bit `openvr_api.dll` in place of the game's own makes them reach the runtime through OpenXR. Build it with
`build-opencomposite.sh`, which applies `opencomposite-mingw.patch` (the OpenVR 2.15.6 interfaces current games ask
for, and the fix that makes a mingw build return matrices the way MSVC-built games expect; the script's header says
what each part is for). It takes under a minute on the Mac:

```sh
./build-opencomposite.sh ~/src/oc-build                                   # -> ~/src/oc-build/openvr_api.dll
./opencomposite.sh install ".../steamapps/common/Half-Life Alyx" ~/src/oc-build/openvr_api.dll   # keeps each original as .orig
./opencomposite.sh restore ".../steamapps/common/Half-Life Alyx"
```

Steam's file check puts the originals back, so run `install` again after a game updates. If a game reports
"Local content might be corrupt", Steam checks its files, deletes the swapped dll and marks the game broken; the game
then also fails with "Need Steam Networking sockets". To recover: quit Steam, set `StateFlags` to `4` and
`UpdateResult` to `0` in `steamapps/appmanifest_<appid>.acf`, start Steam again, and run `install` again.

## Settings that matter for games

- **Render at 1728 per eye, not 2160** (`render_device = "steam_frame"` in the runtime's toml). A game and the
  encoder share the Mac's GPU. At 2160 the encoder's GPU step went from 10 to 24 ms in the busy part of a Beat Saber
  song and dropped about 400 frames in 30 seconds; at 1728 it stayed under 9 ms with 8 drops in 4 minutes. 1728 is
  SteamVR's own recommendation for the Frame. It is a little softer.
- `resolution_scale` is the encode scale, not the render size, and it turns foveated encoding off. Leave it at 1.
- **Half-Life: Alyx: `supersampleRatio=0.81`** in an `opencomposite.ini` beside its OpenComposite dll
  (`alyx-opencomposite.ini`, copied to `game/bin/win64/opencomposite.ini`). It draws 1399 per eye instead of 1728. On
  the first in-world map the full size saturated the GPU and the encoder with it: 29 renders/s and 89 ms frame age.
  At 0.81 it ran 88 renders/s with an 18 ms frame age (measured 2026-10-31, worn).

## UEVR games (flat Unreal Engine games in VR, experimental)

[UEVR](https://github.com/praydog/UEVR), by praydog, injects 6DoF VR into flat Unreal Engine 4 and 5 games. It runs
on this chain through its OpenXR path. **One game works under UEVR: Sifu (UE 4.26.2, the Epic build, 6770)**.
Every other UEVR game is untried here.

**What you need besides the chain above** (all in `uevr/` beside this README):

- **UEVR nightly-01143, pinned.** Take `uevr.zip` from
  [praydog/UEVR-nightly, release nightly-01143-4ee5c6b](https://github.com/praydog/UEVR-nightly/releases/tag/nightly-01143-4ee5c6b6162dee2291fc75f9dfc57667f6d45a2d)
  (38,974,809 bytes, sha256 `4cd6932d1d01168178aef70499d40b101ea6f19868cf76099d8e0087ef8dc6d8`, `revision.txt` =
  `4ee5c6b`). Unzip it into the prefix, for example to `prefix/drive_c/uevr`. Newer nightlies are untested.
- **DXMT with `dxmt-desktop-window.patch`.** UEVR finds Direct3D's functions by creating a throwaway swapchain on the
  desktop window. Under Wine that window belongs to another process, so stock DXMT refuses it and UEVR never starts
  VR (the game stays flat). The patch gives it a hidden window of the game's own instead. Build: run
  `build-dxmt.sh <dir>` once, `git -C <dir>/dxmt apply uevr/dxmt-desktop-window.patch`, run `build-dxmt.sh <dir>`
  again, and install as in step 3. Non-UEVR games are unaffected: no game makes a swapchain on the desktop window.
- **The kernel32 guard-page fix** from step 2. Without it, one of UEVR's memory probes killed a game (Astroneer).
- **`uevr-inject.exe`**, built from `uevr/uevr-inject.c`:
  `x86_64-w64-mingw32-gcc -O2 -municode -o uevr-inject.exe uevr/uevr-inject.c`. It replaces UEVR's frontend,
  `UEVRInjector.exe`, which is a .NET 6 WPF app and would need the .NET Desktop Runtime in the prefix. It does what
  the frontend does for OpenXR: it writes `Frontend_RequestedRuntime=openxr_loader.dll` into the game's UEVR
  `config.txt`, then loads UEVR's `openxr_loader.dll` and `UEVRBackend.dll` into the game.
- **The controller profile**, `uevr/_interaction_profiles_oculus_touch_controller.json`. Copy it to
  `prefix/drive_c/users/<you>/AppData/Roaming/UnrealVRMod/UEVR/Profiles/` (it serves every game). It is UEVR's
  default list plus the d-pad, because the file replaces the defaults.

**Run a game:**

```sh
OXRSYS_FRAME_XBOX=1 ./run-game.sh ".../Sifu/Binaries/Win64/Sifu-Win64-Shipping.exe"   # the store's arguments after it
# when the game reaches its menu (Sifu: about 45 s), from a second Terminal:
WINEPREFIX=$PWD/prefix wine/bin/wine uevr-inject.exe Sifu-Win64-Shipping.exe 'C:\uevr'
```

Then start the headset client as for any game (`frame-stream up --app external`), **after** the inject: the game has no
OpenXR session until UEVR makes one, so an `up` before it finds no live runtime. Add `--nullify` to the inject for a game that ships its own VR
plugin.

**Close UEVR's menu first.** UEVR opens its menu when it injects. While the menu is open, UEVR keeps the gamepad and
the game receives no input. It toggles with Insert, which a Mac keyboard lacks: use a PC keyboard, or press both
thumbsticks in and hold for about a second (L3 + R3, from UEVR's source; not yet tried on the Frame).

**`OXRSYS_FRAME_XBOX=1` lays the Frame's controllers out as an Xbox pad** (runtime patch 0032). Without it,
SteamVR's own Frame-to-Touch mapping folds right B, X and Y into one button and the d-pad into two, and drops the
bumpers and the right menu button. With it, each Frame input lands on the Xbox button a game's prompts name:

| Frame | Xbox |
|---|---|
| A, B, X, Y | A, B, X, Y |
| d-pad | d-pad |
| grips and bumpers | LB / RB |
| triggers | LT / RT |
| stick clicks | LS / RS |
| either menu button | Start (tap) / Back (hold over 0.5 s) |

It acts only when the headset client sends the Frame's per-button bits (client patch 0022). All
19 inputs arrive at the game as the expected button. To see what a game reads, add `OXRSYS_ACTION_TRACE=1` (one log
line per change). `uevr/pad-trace.lua`, copied to `UnrealVRMod/<exe name without .exe>/scripts/`, logs the Xbox pad
UEVR hands the game.

**Sifu's status:**
- Plays in VR, in stereo, with head tracking and the controllers above.
- **Set `VR_RenderingMethod=1` (synchronized sequential)** in `UnrealVRMod/Sifu-Win64-Shipping/config.txt`, before
  injecting or from UEVR's menu. With UEVR's default, native stereo, Sifu freezes about 20 s after injection, 2 runs
  of 2. UEVR logs that its native-stereo fix-up could not patch one spot in Sifu's exe. Synchronized sequential
  renders each eye in turn, so each eye gets a new image every other game frame. On the release runtime
  (render 1728, off-head, 3 launches and a 10-minute run): the game ran at about 64 frames a second
  (about 129 presents, two per frame), 70-75 % of the headset's 90 Hz frames were new, no freeze and no crash.
  About once a minute Sifu itself stalls for up to a second (the runtime waited 1015 ms for its next frame).
- **The menus before gameplay ignore the controllers** ("press any button" included): use the keyboard until the game
  starts.
- **If the world sits too low, reset the standing origin, then recenter.** In UEVR's config, `VR_ResetStandingOriginKey=119`
  (F8) and `VR_RecenterViewKey=118` (F7) put both on keys.
- Sifu is third-person: the camera sits behind the character.

## Known problems

- **Clicks in a Wine window seem dead**: check that nothing covers it. A macOS alert or another app's window over a
  Wine window takes the clicks while the keyboard still reaches Wine. Plain Wine windows, titled and borderless,
  get every click on macOS 27. Steam's window itself is not yet retested; its keyboard
  shortcuts work regardless (`steam.exe steam://install/<appid>`, step 6; Tab and Enter in dialogs).
- The game goes silent when the headset is off your head.
- A level fail in Beat Saber at 2160 froze the game for 2.7 seconds once. It has not recurred at 1728.
- **Half-Life: Alyx** plays at 90 Hz through the patched OpenComposite, with its hands. Its left
  trigger finger twitches now and then. Each frame arrives as a 4254x2127 texture, probably both eyes side by side
  at 2127, since Alyx scales its own resolution. Earlier runtimes gave it no hands, or let it exit by itself a few
  seconds in; runtime patches 0053-0054 fixed both.
- Native OpenXR games need none of this: Beat Saber runs as it is.
- The 64-bit DXMT path only. 32-bit games are untested.

## What has been tried

| Game | Path | State |
|---|---|---|
| Beat Saber | native OpenXR | plays worn, 90 Hz |
| Sifu (Epic) | UEVR | plays worn; synchronized sequential; keyboard for its first menus |
| Pistol Whip | native OpenXR | streams at 90; worn: the game draws no guns yet |
| Astroneer | UEVR | stereo reaches the runtime off-head; not yet played |
| Half-Life: Alyx | OpenComposite | plays worn, 90 Hz, hands and fingers; the left trigger finger twitches now and then |
| Titanfall 2 VR | Northstar | blocked: the EA app does not install under this Wine |
