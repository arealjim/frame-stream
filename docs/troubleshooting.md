# Troubleshooting

Each entry is something that cost us an evening. The symptom first, then the cause, then what to do.

Two commands find most of what follows for you. `bin/frame-stream doctor` checks the setup: both links, which
client and which runtime are really running (against the patches in this copy), the refresh rate, the firewall,
the Mac's temperature and the Wine build. `bin/frame-stream watch`, left running in a second terminal, prints one
line when the picture freezes, the engine quits, the headset stops answering or its house Wi-Fi drops, each with
the cure from this page. Neither changes the headset.

## doctor says the client or the runtime is "not this release's build"

The headset or the Mac is running something other than this copy's patches: an older install, or another build
you made. Run `frame/install.sh` on the headset (for the client) or the runtime build in the README (for the Mac),
then `frame-stream down` and `up`. "Rebuilt after it started" or "changed after it started" means the right build
is on disk but the running app still has the old one loaded: `down`, then `up`. A runtime library's checksum cannot
be compared with ours, because two builds of the same source differ; doctor compares the source instead.

## The headset says "no server found"

The client asks the Mac directly on UDP 9946 and hears nothing back. Three causes, in the order we met them:

1. **The Mac's application firewall.** With stealth mode on, macOS drops an unsolicited packet for any binary that is
   not on its allow list, and logs nothing. The allow list is per executable, and a process reads it when it opens
   its sockets, so add the engine first, then launch it:

   ```sh
   APP="/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor.app"   # or Godot.app, Unity.app
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$APP"
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$APP"
   ```

   The first time an engine opens its ports you may instead get the usual "accept incoming network connections?"
   prompt. Say yes. If you said no once, it stays no until you remove the app from the list in System Settings →
   Network → Firewall.

2. **A per-app firewall (Little Snitch, LuLu and the like).** The Unreal editor from the Epic launcher is ad-hoc signed. A rule like
   "allow local network for apps with a trusted signature" does not cover it, so its packets on the USB and hotspot
   subnets are dropped at the socket filter. Make an explicit rule for UnrealEditor: UDP 9943-9948, to the headset's
   subnet only (`ifconfig` on the Mac shows the tether's address and netmask; ours was a /29). Godot and Unity are signed and pass the usual rules.

3. **The wrong address.** For a while the headset could not reach the Mac's home-LAN address at all (no ping, no
   UDP), though both sat on the same subnet; later, with both on the same access point and band, it could, and a game
   played over it. If yours cannot, use the USB-C tether (`frame-stream up` finds it by itself), the
   headset's own hotspot with the Mac joined to it (`--via-hotspot`), or a routed overlay such as Tailscale
   (`--mac-ip <the Mac's tailnet address>`).

## It runs at 72 Hz, not 90

The Frame runs OpenXR apps at SteamVR's `defaultPerAppRefreshRate`, which is 72 from the factory, whatever the app
asks for. The idle dashboard runs at 90, so the switch happens about a second after the session starts. The client
follows the panel and tells the Mac, so 72 is smooth, just slower. For 90, the rate this release is tuned for, on the headset with SteamVR up:

```sh
frame-vrsettings set steamvr defaultPerAppRefreshRate float 90    # built by frame/install.sh
```

It applies at once and SteamVR saves it. 80 and 72 work too (`float 80`; 72 was tested on an earlier build). Editing `steamvr.vrsettings` by hand does nothing until SteamVR restarts.

## Which link to use

| Link | Worn result | Notes |
|---|---|---|
| USB-C cable | every frame fresh, 0 repaints, arrival jitter 0.5 ms | best |
| The headset's own hotspot, Mac joined ("two links", below) | unworn: 100 % fresh and 0 loss bursts with a Unity scene, where the house path had 25-33 a minute; carries 250 Mbit/s cleanly. Worn: short sessions only | needs the setup below; run `bin/frame-stream check-link` |
| A shared home access point | varies: one session 10-39 % repainted in bunched stretches, another Godot 70-72 of 72 frames fresh, Beat Saber "pretty solid" at 1728 per eye | both on one AP and band (we used 5 GHz); the headset says hello first for sound |
| Tailscale (a routed VPN), unworn | works at 25 Mbit/s (0.2 % loss, against 0.4 % on the same home network direct) | the Mac and headset go direct when they share a network |

Loss on Wi-Fi comes in bursts and changes with the network and the hour; it has shown up worn and unworn. Measure on
your own network (`tools/regress.sh`, or `frame-stream status` while it runs) before you trust a link.

## The cable is in, and there is no tether

`frame-stream up` says "no USB-C tether address found", or `doctor` says the headset "is on the cable but macOS has
not let it connect". macOS holds a new USB accessory back while the Mac is locked: the headset charges, but no network
appears. Unlock the Mac, unplug the cable and plug it in again, and answer **Allow** if macOS asks (System Settings >
Privacy & Security > Allow accessories to connect). The tether then comes up in a few seconds; `ifconfig` shows it as
an `en` interface with a `10.86.200.x` address (ours; set `FRAME_STREAM_USB_NET` if yours differs).

## Two links: the hotspot for the Mac, the house Wi-Fi for the headset

The headset can host a Wi-Fi hotspot and stay on your house Wi-Fi at the same time, on one radio. Put the Mac on
the hotspot and the stream gets a link of its own: none of the house network's other traffic is on it, and the
headset keeps its internet. On our unit the hotspot runs on 6 GHz (160 MHz wide), and the house link was on 5 GHz,
later on 2.4 GHz.

What it needs, all checked by `bin/frame-stream check-link` (read-only; it prints the fix for anything it finds):

1. **The hotspot on, the Mac joined to it.** The hotspot is hidden, and by default Steam manages its name
   (`frame_` and eight characters) and a 32-character password. To set your own, in the headset: Steam (the logo at
   the left of the overlay's bottom bar) → Steam Settings → Remote Play → the "Wireless Streaming Adapter (Steam
   Frame)" section → Connection Management: Custom, then type a Hotspot SSID and Hotspot Password and restart the
   headset. Join the Mac with that name and password (it is hidden, so "Other network..."). The section and its
   Steam/Custom choice are as we found them on SteamOS 0.5.6; the restart step is from the
   [Gentoo wiki's Steam Frame page](https://wiki.gentoo.org/wiki/Steam_Frame). Menus may move between SteamOS
   updates. A Mac without Wi-Fi 6E (the M1 Macs, the M2 MacBook Air) may not see a
   6 GHz hotspot at all; we have tested only a 6E Mac, so use the USB-C cable. Then see "The Mac does not see the hotspot" below.
2. **The house link off DFS channels, and off the hotspot's band.** Both links on one band most likely take turns on
   the air. And the headset's radio runs two channels at once only when neither needs radar detection, so a house
   network on a DFS channel (5 GHz channels 52-144) makes it time-share too: streams here had 1-2 s blackouts in that
   state. Put the house access point on channels 36-48 or 149-165, or pin the house connection to 2.4 GHz, or turn the
   headset's house Wi-Fi off while streaming. On the headset: `sudo nmcli con modify '<house network>' 802-11-wireless.band bg`
   pins it to 2.4 GHz. `802-11-wireless.band ""` undoes it.
3. **The house link locked to one access point.** NetworkManager scans in the background to roam, and a scan takes
   the radio off the hotspot's channel: the stream stalled for up to 2 s each time (measured). Lock the
   connection to your access point's BSSID, which you can read with `iw dev wlan0 link`, and roaming scans stop:
   `sudo nmcli con modify '<house network>' 802-11-wireless.bssid <BSSID> && sudo nmcli device reapply wlan0`.
   `802-11-wireless.bssid ""` undoes it. Opening the Wi-Fi list in the headset's settings still scans, and still
   stalls the stream for that long. Redo this on every new house network.
   **Steam can undo both pins.** After a reboot, Steam re-created the house connection as a new profile with
   neither the band pin nor the lock (seen once). `frame-stream check-link` reports both, so run it after a
   reboot and redo the pins if it warns.
4. **AWDL off on the Mac.** AirDrop's radio takes the Mac's Wi-Fi off channel. It gave 150-200 ms hiccups here,
   and none with it down: `sudo ifconfig awdl0 down`. macOS raises it again after any Wi-Fi restart.

Measured unworn, with the house link on 2.4 GHz and the hotspot on 6 GHz: loading the house link to 110 Mbit/s
down, or 97 up, cost the stream nothing (100 % of frames fresh). If the headset loses the house Wi-Fi, though, the
stream keeps going but hitches (up to 2 s) until it rejoins.

Internet on the Mac: by default the hotspot does not route out, so the Mac has none while it is joined. Under
Connection Management: Custom there is also "Hotspot Client Internet"; set it to "NAT (via HMD Wi-Fi)" and the Mac
gets a router, DNS and the internet through the headset's house Wi-Fi, at once, with no restart. On our unit,
the stream with nothing else running read the same as without NAT (52.1 ms, 80 fresh frames a second,
99 % fresh). A download on the Mac shares both of the headset's links. A light to moderate one does not disturb the
stream: with about 18 Mbit/s running for two 2 min runs the stream read 51.9 and 53.2 ms, 99.4 and 98.3 % fresh,
no reprojected frames, and a 30 s burst at about 380 Mbit/s showed no visible hit either. Heavy sustained downloads
are untested; pause one if the picture stutters. If you run a firewall on the headset, allow forwarding on the tether and
hotspot as well; a stock Frame should need only the setting (not tested). Without NAT, a USB Ethernet
adapter or an iPhone on USB gives the Mac internet. On our Mac the hotspot link also went silent now and then (still associated,
nothing passing) until the Mac rejoined it: pick the hotspot again in the Wi-Fi menu.

## The Mac does not see the hotspot

The hotspot is hidden, so the Mac only finds it if it asks for it by name. If the Mac's saved profile for it says
it is not hidden, the Mac never asks: the network is in no list, and every join fails. Join it once with Wi-Fi →
"Other Network…", typing the name. If it still will not rejoin later, the saved profile is the likely cause:
remove the network in System Settings → Wi-Fi → Known Networks, then join it again as hidden.
`frame-stream check-link` reads the flag when it can use sudo.

If the Mac left the hotspot and cannot find it again while the headset still shows it on, restarting the headset
cured it here.

## The view shakes violently

Two clients are connected to one runtime and their head poses are interleaved. Usually it is the OXRSys Simulator,
started with `--auto-connect`, left running beside the headset. `frame-stream up` refuses while the simulator runs. The
runtime patch in this repository refuses a second Wi-Fi client while one is connected.

## The engine starts and then nothing happens

- **Unity, first launch on this Mac:** a software-terms dialog waits on the Mac's screen, with nothing in any log.
  Launch the editor by hand once and accept it.
- **The Unity editor quits in its first minute:** a stack overflow in the editor's own analytics uploader, Unity's
  code, not this project's; seen 4 times on the Mac we used. `frame-stream up --app unity` starts it once more by
  itself, and says so. Switching off Editor analytics for your Unity account stops it: Preferences → General →
  Disable Editor Analytics, or Unity ID → My Account → Privacy → Editor.
- **Unity, after a crash or a kill:** the next launch sits at 0 % CPU behind two dialogs ("project already open",
  "recover scene backup"). `frame-stream down` removes `Temp/UnityLockfile` and `Temp/__Backupscenes` for you; after a
  crash, delete them by hand.
- **Unity, with the Mac's display asleep:** the app submits 2 frames, reaches the XR state VISIBLE and stops there,
  with no error. Seen on every launch of a Unity player build made while the display slept, with any settings and with
  or without the headset connected; with the display held on, the same launch reached FOCUSED in 0.25 s. Godot and
  Unreal are not affected. Wake the display and hold it on for the session: `caffeinate -u -d` in a Terminal.
- **A GUI app started from ssh** spins without a window. Start it through `open` (`frame-stream` does).
- **"Failed to bind control socket on port 9946"**: a wedged earlier process still holds UDP 9944-9946, and the
  runtime never retries a bind. `sudo lsof -nP -iUDP:9944-9946`, then `kill -9` it.

## Settings in the toml seem to be ignored

The runtime reads `~/Library/Application Support/OXRSys/oxrsys-runtime.toml` if it exists, and only otherwise the toml
beside the runtime library. The simulator's settings panel creates the first one. `frame-stream status` prints the
file it is actually using, and `frame-stream up --bitrate/--ffe/--render` edits that one. Every run's effective
settings are printed in the runtime log's startup lines: read those before comparing two runs.
`frame-stream doctor` names the file too, and lists the keys a short override lacks; those keys run on the
runtime's defaults.

## The picture is black with Godot on Metal

Godot's Metal renderer asks for a mutable-format swapchain, which the runtime rejected until
[OXRSys #54](https://github.com/demonixis/oxrsys/pull/54). Use `--rendering-driver vulkan` (`frame-stream` does), or
apply the runtime patches here.

## The headset shows no picture with one app, and other apps work

The runtime log says `dropping frame before encode: invalid foveated texture, dimensions, usage, or settings`. The
app renders a smaller eye than the stream's 2160x2160 (Cyberpunk's VR port draws 1912x1928, and a game's
resolution or render-scale setting can shrink it), and with foveated encoding on, a runtime without patch 0033
drops every such frame. Apply the runtime patches here (0033). For a runtime you cannot patch, `frame-stream up
--ffe off` streams it unfoveated. With 0033 the log says once `foveation scales the app's WxH eye to the 2160x2160
target eye`, and the picture is as sharp as the app's own render.

## The picture looks washed out

Older client builds encoded the gamma twice (lifted blacks, flat contrast). Rebuild with
`frame/install.sh`. To compare, `FRAME_CLIENT_SRGB_LINEARIZE=0 frame-stream up ...` shows the old picture.

## The runtime build fails on the Mac

- **"unknown architecture" or a link error naming `/Library/Developer/CommandLineTools`**: CMake found the Command
  Line Tools' compiler or SDK instead of Xcode's. Name them, as the README's build line does
  (`-DCMAKE_C_COMPILER`, `-DCMAKE_CXX_COMPILER`, `-DCMAKE_OSX_SYSROOT`).
- **"Could not find OXRSYS_VULKAN_INCLUDE_DIR"**: `brew install vulkan-headers` and pass
  `-DCMAKE_PREFIX_PATH=$(brew --prefix vulkan-headers)`.
- **A Metal shader step fails**: `xcodebuild -downloadComponent MetalToolchain` (about 700 MB, once).

## No sound in the headset

- **The game plays from the Mac's speakers.** Wine ignores BlackHole's name and its CoreAudio UID as
  `DefaultOutput`; it wants the endpoint id from the prefix's `MMDevices\Audio\Render` key (`wine/README.md`, step 7).
- **The sender runs but the headset hears silence.** macOS gives a process silence, without an error, when it lacks
  the microphone permission, and a sender started over ssh has none. Start `frame-audio-tx` from Terminal and allow it.
- **Over Wi-Fi nothing arrives.** The headset's firewall drops unsolicited inbound traffic on Wi-Fi. Start the
  receiver with the Mac's address (`frame-audio-rx start <mac-ip>`): it says hello first and the sound comes back as
  the reply. The cable is not filtered.
- **The game goes quiet when you take the headset off.** That is the game, not the stream.

## The sound crackles

Every crackle we heard was a gap in the stream, and each had a different cause. ffmpeg's avfoundation capture
delivered 43,000 of 48,000 samples a second with a game loading the Mac, ffmpeg's UDP output sent the sound in 85 ms
bursts, and a pipe into `pw-play` left holes. The scripts here use sox to capture, the system `nc` to send and
ffmpeg's pulse output on the headset. To see gaps rather than guess, record what the headset actually plays:
`pw-record -P stream.capture.sink=true --target $(pactl get-default-sink) out.wav` (recording the stream's own node
gives zeros).

## A game stutters, or runs well and then hitches

Look at the encoder before the network: the runtime log's `encode queue` lines give the GPU time per frame and a
running `encDrops` count. A game and the encoder share the Mac's GPU, and at 2160 per eye a busy Beat Saber song
pushed the encoder from 10 to 24 ms a frame, past the 11 ms budget at 90 Hz. Set `render_device = "steam_frame"`
(1728 per eye) in the toml. If `encDrops` stays flat while it stutters, the game itself stopped producing frames.

## It judders, and every counter looks fine

SteamVR fell to half rate: it shows each frame twice, and nothing on the Mac or in the client's log says so. Run
`frame-stream bench` and read its SteamVR line. Over 1 % reprojected is this. On the stand it came when the
headset's own GPU time ("headset app GPU" in the same line) reached about 10 ms, well before the 11.1 ms frame at
90 Hz, so watch that figure rather than the frame budget. A heavier scene on the Mac did not move it in our runs;
the client's un-warp shader did, and it now takes 3.7 ms. If you see it, send the bench line in an issue.

## The picture freezes on the first frame and stays frozen

The headset shows one picture that follows your head but never changes, while the app on the Mac runs normally.
The headset client got its first frame and then stopped receiving or decoding video. With the lab build this
release came from it happened in about one start in three; on the release build it was not seen in 15 starts. This release's client has a watchdog: with no new video for 2 s it reconnects,
and the picture should come back within a few seconds. If its receiving is stuck for good, the client exits after
2.5 s and systemd restarts it (about 6 s in all). If the picture still stays frozen, run `bin/frame-stream down`,
then `up` again. `frame-stream watch` notices a freeze within about ten seconds and says so.

## After a Godot crash, `up` says "the runtime did not come up in 20 s"

When an app crashes, macOS asks whether to reopen its windows the next time it starts, in an alert you cannot see
from the headset, and Godot waits on it. `frame-stream up` starts Godot with `-ApplePersistenceIgnoreState YES`,
which skips the alert. If you start Godot yourself, add the same argument, or answer the alert on the Mac's screen.

## Godot quits during a session

Godot 4.7 on the Mac (Vulkan through MoltenVK) sometimes loses its GPU: "Vulkan device lost" in its log, then it
quits. It happened 6 times in about 400 test sessions (the sixth worn, 13 s after a button test), as early as a second into one, and once with no headset
connected. macOS's own report calls it a "firmware-detected lockup" with the same signature each time. The cause is
not known, but it has only happened on Vulkan. The headset no longer stays on the last picture: it goes back to
SteamVR home within 20 s (see the next entry). Run `frame-stream up --app godot` again.

To try Godot on Metal instead: `FRAME_STREAM_GODOT_DRIVER=metal frame-stream up --app godot`. It needs MSAA off in
the project (Project Settings > Rendering > Anti Aliasing > MSAA 3D: Disabled), because Godot's Metal driver
crashes on VR with MSAA, and frame-stream refuses to start it otherwise. On the stand it ran like Vulkan (90 Hz,
0 % reprojected), but it has not run long enough to show that it avoids the hang. `engines/godot` ships with
MSAA off, so it runs on Metal as it is.

## Godot logs "needs the TEXTURE_USAGE_STORAGE_BIT usage flag" every frame

```
ERROR: Image (binding: 0, index 0) needs the TEXTURE_USAGE_STORAGE_BIT usage flag set in order to be used as uniform.
ERROR: Uniforms were never supplied for set (1) at the time of drawing, which are required by the pipeline.
```

Your project has MSAA on and submits depth (`openxr/submit_depth_buffer=true`) on the Forward+ renderer, which is
what `frame-stream` runs. With MSAA, Forward+ resolves depth with a compute shader that writes straight into the
OpenXR depth swapchain (Godot 4.7.2: `render_forward_clustered.cpp:2154` calls `resolve_depth`, which binds the
destination as a storage image in `effects/resolve.cpp`). Godot makes that swapchain image without storage usage
(`modules/openxr/extensions/platform/openxr_vulkan_extension.cpp`), so Godot refuses the pass every frame and the
depth it hands the runtime is never written. With `--depth on`, the headset then moves the picture by depth the app
never rendered. This is Godot's bug; the runtime cannot fix it.

Fix it in the project: MSAA 3D off, with FXAA for the edges (Project Settings > Rendering > Anti Aliasing:
MSAA 3D Disabled, Screen Space AA FXAA). `engines/godot` ships like that: 0 of these errors in a
60 s run, against 10,800 (90 frames/s, two eyes) with MSAA 2x, both at a steady 90 Hz. The alternative is to
stop submitting depth (`openxr/submit_depth_buffer=false`) and keep MSAA, which loses head translation.

The error is not what makes Godot quit (previous entry): the Godot editor that `frame-stream` runs refuses the pass
before it reaches the GPU, and the Mac-only launch loops, which ran with this error on every frame, never lost the
GPU. An exported game is built without those checks; export with MSAA off too.

## The game quit and the headset went back to SteamVR home

That is expected. When the app on the Mac quits (the "Real Life" button in the Unreal VR Template's menu, or closing
Godot), the Mac tells the headset the stream has ended. The headset waits 5 s in case the app starts a new session,
then ends its own, and SteamVR shows its home. If the app crashes, that message never comes, so the headset goes
home after 20 s with no video. Start the next game with `frame-stream up` as usual.

The 20 s is `FRAME_CLIENT_APP_GONE_S`: set it on the Mac when you start, for example `FRAME_CLIENT_APP_GONE_S=60 frame-stream up --app godot` (frame-stream passes every `FRAME_CLIENT_` setting to the headset; 0 turns the 20 s rule off).

## The picture freezes for a moment, then catches up

Wi-Fi loses packets in bursts, and a frame the client cannot repair is dropped; it then shows nothing new until a
keyframe arrives, and asks the Mac for one every 120 ms until it does (`FRAME_CLIENT_KEYFRAME_RETRY_MS`). A loss
should cost about 0.1 s. Count them on the headset: `journalctl --user -u oxrsys-client | grep -c 'requested
keyframe'` (and `still discarding` for a keyframe that was itself lost). Many a minute means a lossy link: keep the
Mac close to the router or on Ethernet, and check that the toml in use has `send_pacing_ms` and
`send_pacing_keyframe_ms` (without them each frame leaves the Mac as one burst). The runtime reads
`~/Library/Application Support/OXRSys/oxrsys-runtime.toml` first when it exists, so a short file there overrides
the full one beside the runtime.

## A short hitch about once a minute, for a few minutes each hour

Measured on the hotspot setup ("Two links"): a stall of about 200 ms once a minute, six minutes in a row, about
once an hour, and clean in between. Our home router sent the headset's house-Wi-Fi link a burst of roaming
requests (802.11v BSS transition management) on that schedule, about every 65 minutes, and each one costs the shared radio about 200 ms.
The headset logs them: `journalctl -u wpa_supplicant --since '-2h' | grep 'WNM: Preferred List Available'`
(the unit name may differ). If the times
match your hitches, look in your router's settings for band steering, "fast roaming", 802.11v or BSS transition,
and turn it off for the headset if it allows that. We have not yet tested turning it off on the headset itself.
`frame-stream doctor` counts these requests over the last 24 hours, and `frame-stream watch` reports each as it
arrives, so you can match them to a hitch.

## A Windows game quits right after it starts

It checked that Steam launched it. Run `wine/run-steam.sh` first and log in, and start the game with
`wine/run-game.sh`, which sets the game's Steam app id (or set `STEAM_APPID`).

## A Windows game hangs on launch, at almost no CPU

Some games (seen with Astroneer) hang before their window opens on every launch after the first. It happens
whether the last run was killed or quit cleanly. The cause is Apple's Metal shader cache, which DXMT keeps
beside its own. `wine/run-game.sh` starts every launch without it, so the first frame can take about 20 s while
shaders compile; set `DXMT_KEEP_METAL_CACHE=1` to keep it for a game that does not hang. By hand: move aside
the `com.apple.metal` folder inside the folder named after the game's exe, under
`$(getconf DARWIN_USER_CACHE_DIR)dxmt/`. This does not cure every case yet: about one launch in three still
stalls the same way. Quit it and launch again.

## A game crashes about 20 seconds after UEVR injects

UEVR "Exception occurred: 80000001" in its log is stock Wine's kernel32 missing a fix. Build and install the patched
kernel32 (the Wine README, step 2).

## Where the logs are

| What | Where |
|---|---|
| The headset client | `frame-stream log` (`-f` to follow), or `journalctl --user -u oxrsys-client` on the headset |
| Godot / Unity / Unreal on the Mac | `~/Library/Logs/frame-stream/` |
| The runtime inside Unreal or your own app | `~/Library/Application Support/OXRSys/oxrsys-runtime.log` |
| The runtime's state | `~/Library/Application Support/OXRSys/runtime_status.json` |

The client's true drop count is in its receive thread's exit line:
`ssh frame journalctl --user -u oxrsys-client | grep 'receive thread ended'`.
