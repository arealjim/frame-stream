#!/bin/bash
# SPDX-License-Identifier: MIT
# opencomposite.sh install|restore|status <game-dir> [opencomposite-openvr_api.dll]
# OpenVR games (most SteamVR titles) load openvr_api.dll from their own folders. install puts OpenComposite's 64-bit
# build (OpenVR -> OpenXR, https://gitlab.com/znixian/OpenOVR) in place of every 64-bit copy under <game-dir>,
# keeping each original as openvr_api.dll.orig; restore puts the originals back. 32-bit copies are left alone.
# The DLL defaults to OPENCOMPOSITE_DLL. Steam's file check restores the originals, so run it again after an update.
set -euo pipefail
cmd=${1:?usage: opencomposite.sh install|restore|status <game-dir> [dll]}; dir=${2:?game dir}
oc=${3:-${OPENCOMPOSITE_DLL:-}}
is64() { file -b "$1" | grep -q "PE32+"; }
n=0
while IFS= read -r -d '' f; do
  case $cmd in
    install)
      [ -f "$oc" ] || { echo "no OpenComposite dll (pass it, or set OPENCOMPOSITE_DLL)" >&2; exit 2; }
      if [ -f "$f" ] && cmp -s "$f" "$oc"; then echo "already: $f"
      elif [ -f "$f.orig" ]; then
        # a Steam update writes a new Valve dll over ours: keep it as the original, not the older .orig (review L2).
        # An OpenComposite build names itself in its strings; Valve's does not.
        if is64 "$f" && ! LC_ALL=C grep -a -q -i opencomposite "$f"; then mv -f "$f" "$f.orig"; echo "new original kept: $f.orig"; fi
        cp "$oc" "$f"; echo "updated: $f"; n=$((n+1))
      elif is64 "$f"; then
        mv "$f" "$f.orig"; cp "$oc" "$f"; echo "installed: $f"; n=$((n+1))
      else echo "skipped, 32-bit: $f"; fi ;;
    restore)
      if [ -f "$f.orig" ]; then mv -f "$f.orig" "$f"; echo "restored: $f"; n=$((n+1)); fi ;;
    status)
      if [ ! -f "$f" ]; then echo "MISSING:       $f (only the .orig is left)"
      elif [ -f "$f.orig" ]; then echo "OpenComposite: $f"; else echo "original:      $f"; fi ;;
    *) echo "usage: opencomposite.sh install|restore|status <game-dir> [dll]" >&2; exit 2 ;;
  esac
done < <(find "$dir" \( -iname openvr_api.dll -o -iname openvr_api.dll.orig \) -print0 |
  while IFS= read -r -d '' g; do printf '%s\0' "${g%.orig}"; done | sort -zu)
# an .orig alone (the dll itself deleted, by hand or by Steam) still counts: install writes the dll back
[ "$cmd" = status ] || echo "$cmd: $n file(s)"
