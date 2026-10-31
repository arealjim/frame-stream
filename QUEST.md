# Trying frame-stream with a Meta Quest 3

frame-stream was built and tuned on a Steam Frame. Nobody has run it with a Quest yet. If you have an **Apple
Silicon Mac** (any M1 to M5) and a **Meta Quest 3 or 3S**, you can be the first, and you do not need to be a
developer: one script does the setup and the test, and asks before it changes anything.

**What you do:** turn on the Quest's developer mode, install Xcode, then run one command and wear the headset for two
5-minute sessions. **What you send back:** one zip file the script makes on your Desktop. **Time:** an afternoon the
first time, most of it waiting for downloads and builds. Your part in the headset is about 15 minutes.

**What it tests.** OXRSys, the open-source Mac VR runtime frame-stream is built on, already streams to a Quest with
its own headset app. frame-stream is OXRSys plus about 50 changes to the Mac side: pacing to the headset's display,
a steadier head-pose timing, an encoder quality floor, menus and HUDs composited before the encode, and more. The
script runs the same scene twice, once with stock OXRSys and once with frame-stream, and asks you how each felt. We
want to know three things: does the Quest connect to frame-stream at all, does it hold 90 frames a second, and is it
smoother than stock.

A failure is a useful result. If the Quest never shows a picture, the logs in the zip tell us why, and that is the
answer we most need.

## What you need

- an Apple Silicon Mac with about 25 GB free (Xcode is most of it) and macOS 15 or newer;
- a Meta Quest 3 or 3S, charged, with its USB-C cable (a USB-C to USB-C cable for a Mac without USB-A);
- a phone with the Meta Horizon app, signed in to the account the Quest uses;
- a clear play area: the test asks you to look around and take a step or two.

## Step 1: developer mode on the Quest

This lets the Mac install the test app on the Quest over the cable. Meta asks for two things first, once per
account:

1. **A developer team.** Go to https://developers.meta.com/horizon/manage/organizations/ , sign in with your Meta
   account and create a team (any name; it is free).
2. **Account verification.** On the same site, verify by text message (your phone number) or by adding a card. Meta's
   page with all of this: https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/

Then, in the Meta Horizon app on your phone: **Devices → your headset → Headset settings → Developer mode → on**.
Restart the Quest.

You can turn developer mode off again after the test, the same way.

## Step 2: Xcode

Install **Xcode** from the Mac App Store. It is free and large, so start it first and do step 3's reading while it
downloads. Open it once when it finishes and let it install its components, then quit it.

## Step 3: get frame-stream and run the test

Open **Terminal** (Applications → Utilities → Terminal). Paste these lines one at a time and press Return after each:

```sh
git clone https://github.com/arealjim/frame-stream ~/src/frame-stream
cd ~/src/frame-stream
quest/quest-test.sh
```

(If the first line says the command line developer tools are needed, click **Install**, wait, and paste it again.)

The script works through six steps, says what it found at each one, and asks `[y/N]` before it installs or changes
anything: type `y` and Return to go ahead. If it stops, it says why. Fix that and run `quest/quest-test.sh` again:
the steps it has finished are skipped.

1. **The Mac side.** It runs `./install.sh --quest`, which installs Homebrew (a tool that installs other tools) and
   three small build tools from it, downloads OXRSys and applies frame-stream's changes, builds and tests it, and
   downloads Godot (the free game engine the test scene runs in) into Applications. This is the long part. Some
   steps ask for your Mac password.
2. **Stock OXRSys.** A second copy of OXRSys without our changes, built next to the first, for the comparison.
3. **The Quest.** It installs `adb`, Google's tool for talking to Android devices over USB, then asks you to plug
   the Quest into the Mac. The first time `adb` runs, macOS asks whether to open it because it came from the
   internet: choose **Open** (the window can hide behind Terminal; until you answer it, the script waits). **Put the headset on**: it asks once whether to allow USB debugging from this computer.
   Tick "Always allow" and choose Allow.
4. **The Quest app.** It downloads OXRSys's own Quest app (about 3 MB), checks its checksum and installs it on the
   Quest over the cable. It is the unchanged open-source app from the same OXRSys version, built by us so you do not
   need Android's developer tools. If you already have an OXRSys app on the Quest, the script asks before replacing
   it.
5. **Two runs, 5 minutes each.** Stock first, then frame-stream. For each run the script starts the app on the Quest
   and the test scene on the Mac, and waits until they connect. Then it tells you to put the headset on. Look around,
   turn your head quickly a few times, step sideways, lean towards the cube and the panels, and point and click with
   the controllers. Then look at one spot and keep still for a while. The Mac says "run finished" out loud at the
   end. Take the headset off and answer a few questions in Terminal: did a picture appear, how smooth was it, any
   judder or freezes, did the controllers work. Short answers are fine.
6. **The zip.** It makes `frame-stream-quest-results-<date>.zip` on your Desktop.

Keep the cable in for the whole test. The stream runs over it, so your Wi-Fi does not matter.

## Step 4: send it back

Open an issue at https://github.com/arealjim/frame-stream/issues and attach the zip (drag it onto the text box), or
send it to whoever gave you this page.

**What is in the zip**, so you can look before you send it (it is plain text):

- the runtime's log for each run (`oxrsys-runtime.log`) and the test scene's log (`godot.log`);
- the Quest app's own log lines and the headset's frame-rate lines (`quest-app.log`, `quest-framerate.log`). Only
  those lines are kept from the headset's log, nothing else from it;
- the settings each run used (`settings.toml`) and your answers (`answers.txt`, `compare.txt`);
- `system.txt`: your Mac's model, chip and memory, the macOS version, and the headset's model and software version.

The logs contain your Mac's user name (in file paths) and its address on your home network (like 192.168.1.20,
which is not reachable from the internet). Nothing is sent anywhere by the script: you send the zip.

## If something goes wrong

| What you see | What to do |
|---|---|
| It stops after "if nothing happens after this line" | macOS is asking whether to open `adb`: find that window (try Cmd-Tab, or move Terminal aside) and choose Open. |
| "no headset on adb yet" | Developer mode on (step 1), Quest restarted, cable in. Try another cable: some are charge-only. |
| "has not allowed it yet" | Put the headset on and accept "Allow USB debugging". If you missed it, unplug and plug the cable back in. |
| "no connection after 90 s" | The run ends and keeps its logs. The headset may have been asleep: put it on during the wait next time. Run again with `quest/quest-test.sh --only stock` or `--only frame-stream`. |
| A black or frozen picture in one run | That is a result: answer the questions as it was and carry on. |
| A build failed | Send us the Terminal output (select it all, copy, paste into the issue). |
| Godot asks to reopen windows, or quits | Run that run again with `--only`. Godot on a Mac occasionally loses its graphics device. |

`quest/quest-test.sh --check` lists what is missing and changes nothing. `quest/quest-test.sh --pack` makes the zip
again. `--minutes 10` makes each run longer.

## Afterwards: removing it

- On the Quest: Library → Unknown Sources → OXRSys → uninstall, and turn developer mode off in the Meta Horizon app
  if you like.
- On the Mac: delete `~/src/frame-stream`, `~/src/oxrsys`, `~/src/oxrsys-stock` and
  `~/Library/Application Support/OXRSys`. Godot is in Applications. Homebrew and Xcode stay unless you remove them;
  `brew uninstall --cask android-platform-tools` removes `adb`.

## For developers

- The Quest app is `clients/android-vr` from OXRSys commit `b41d892` (the commit frame-stream's runtime patches sit on),
  built with `./gradlew assembleRelease`, unchanged, signed with an Android debug key. sha256
  `2938511aa8d3fb6e847e056df5ca9e13f7e516e5e38265004a1ef0828eecbf38`. To use your own build of it, set
  `QUEST_APK=/path/to/app-release.apk` before running the script.
- The four settings the script changes for a Quest are in the README, under "Other headsets (untested)". The
  script writes them into the runtime's `oxrsys-runtime.toml`; stock runs as OXRSys ships it, at 90 Hz.
- With the setup done, any OpenXR app can use the patched runtime: start it with
  `XR_RUNTIME_JSON=~/src/oxrsys/build/macos-arm64/runtime/oxrsys-runtime.json`, after
  `adb reverse tcp:9944 tcp:9944` (and 9945, 9946, 9948) and the OXRSys app opened on the Quest. Over Wi-Fi the
  Quest app finds the Mac by itself when the cable is out.
- `bin/frame-stream` and the rest of the README are Steam Frame tools and do not drive a Quest.
