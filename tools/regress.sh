#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# regress.sh - one line of numbers per engine for the Mac -> Steam Frame chain. Run it on the Mac from the repository
# root, with the headset reachable over ssh and SteamVR up on it (wearing it is not needed). For each engine it runs
# bin/frame-stream up, lets the stream run, reads the runtime's pacing and latency lines on the Mac and the client's
# counters on the headset, then bin/frame-stream down. About 2.5 minutes per engine.
#   tools/regress.sh [godot|unity|unreal ...]      default: all three
#   env REGRESS_SECONDS=60   seconds attached per engine
#   env FRAME_STREAM_MAC_IP  the address the client is pointed at (default: frame-stream's, the USB-C tether's)
#   env FRAME_STREAM_FRAME   ssh destination of the headset (default: frame)
# Columns, and the lines they come from: late = "app late N" summed over the runtime's last three WaitFrame pacing
# lines (90 frames each); render / enc+send / disp age / rtt / lost = the runtime's last "sample->display latency
# (measured)" line (render ms, encode+send ms, client displayed age ms, rtt median ms, lost/total packets);
# encDrops = the last "encode queue" line; fresh = the client's last hidden-loop "N fresh frames" (per 8 s; 720 at
# 90 Hz); warp = the client's "warp stamped->displayed mean/max" deg; d/k/l = client drop lines / keyframe requests /
# connection lost in the window. With the headset unworn, "sample age at fetch" is a canned pose and is not shown.
set -u
cd "$(dirname "$0")/.." || exit 1
[ -x bin/frame-stream ] || { echo "run this from a frame-stream checkout (no bin/frame-stream)" >&2; exit 1; }
apps=("$@"); [ ${#apps[@]} -eq 0 ] && apps=(godot unity unreal)
secs=${REGRESS_SECONDS:-60}
FS=./bin/frame-stream
FRAME=${FRAME_STREAM_FRAME:-frame}
LOGS=$HOME/Library/Logs/frame-stream
unset FRAME_CLIENT_DUMP
printf '%-7s %-5s %-7s %-9s %-9s %-6s %-9s %-9s %-9s %-14s %-8s %s\n' app late render enc+send "disp age" rtt lost encDrops fresh/8s "warp mean/max" d/k/l took
for app in "${apps[@]}"; do
  t0=$(date +%s)
  out=$($FS up --app "$app" --bitrate 50 --ffe medium_plus --render steam_frame_native 2>&1) || { printf '%-7s FAILED: %s\n' "$app" "$(echo "$out" | grep -E "frame-stream:" | tail -1 | cut -c1-120)"; continue; }
  sleep "$secs"
  case $app in godot) L=$LOGS/godot.log;; unity) L=$LOGS/unity.log;; *) L="$HOME/Library/Application Support/OXRSys/oxrsys-runtime.log";; esac
  late=$(grep 'WaitFrame pacing' "$L" | tail -3 | grep -oE 'app late [0-9]+' | awk '{s+=$3} END{print s+0}')
  lat=$(grep 'sample->display latency (measured)' "$L" | tail -1)
  render=$(grep -oE 'render [0-9.]+' <<<"$lat" | awk '{print $2}')
  enc=$(grep -oE 'encode\+send [0-9.]+' <<<"$lat" | awk '{print $2}')
  disp=$(grep -oE 'client displayed age [0-9.]+' <<<"$lat" | awk '{print $4}')
  rtt=$(grep -oE 'rtt [0-9.]+' <<<"$lat" | awk '{print $2}')
  lost=$(grep -oE 'lost [0-9]+/[0-9]+' <<<"$lat" | awk '{print $2}')
  ed=$(grep 'encode queue' "$L" | tail -1 | grep -oE 'encDrops=[0-9]+' | cut -d= -f2)
  cl=$(ssh -o ConnectTimeout=8 "$FRAME" "L=\$(journalctl --user -u oxrsys-client --no-pager -o cat --since '-${secs}s'); fresh=\$(grep -oE '[0-9]+ fresh frames' <<<\"\$L\" | tail -1 | grep -oE '^[0-9]+'); warp=\$(grep -oE 'warp stamped->displayed mean [0-9.]+ max [0-9.]+' <<<\"\$L\" | tail -1 | grep -oE '[0-9.]+ max [0-9.]+' | sed 's/ max /\\//'); echo \"\${fresh:-?} \${warp:-?} \$(grep -cE 'dropped \\(' <<<\"\$L\")/\$(grep -cE 'requested keyframe' <<<\"\$L\")/\$(grep -cE 'connection lost' <<<\"\$L\")\"")
  read -r fresh warp dkl <<<"$cl"
  # No fresh-frame count from the client means it never connected, and the Mac numbers would be from an older run.
  if [ "${fresh:-?}" = "?" ]; then printf '%-7s FAILED: the client reported no fresh frames (never connected?)\n' "$app"; $FS down >/dev/null 2>&1 || true; continue; fi
  $FS down >/dev/null 2>&1 || { echo "   (down failed for $app - stopping)"; exit 3; }
  printf '%-7s %-5s %-7s %-9s %-9s %-6s %-9s %-9s %-9s %-14s %-8s %s\n' "$app" "${late:-?}" "${render:-?}" "${enc:-?}" "${disp:-?}" "${rtt:-?}" "${lost:-?}" "${ed:-?}" "$fresh" "$warp" "$dkl" "$(( $(date +%s) - t0 ))s"
done
