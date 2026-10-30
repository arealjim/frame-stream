# SPDX-License-Identifier: MIT
# pin-check.sh - sourced by the launchers: refuse to start a Wine this recipe was not built for.
# winemac-dxmt_compat.c reads winemac's private window data, which Wine is still reshaping and does not promise to
# keep, so on another version DXMT fails at runtime ("Failed to create metal view"), not at build. FRAME_WINE_ANY=1
# skips the check, for trying a newer Wine on purpose.
pin_check() { # pin_check <wine dir>
  local w=$1 want=wine-11.16 got so
  [ "${FRAME_WINE_ANY:-0}" = 1 ] && return 0
  got=$("$w/bin/wine" --version 2>/dev/null) ||
    { echo "pin-check: no Wine at $w (README step 1)" >&2; return 2; }
  case $got in "$want"|"$want "*) ;; *)   # "wine-11.16" or with a build tag after a space
    echo "pin-check: this recipe needs $want, found $got (README step 1). FRAME_WINE_ANY=1 to try anyway." >&2
    return 2;; esac
  so=$w/lib/wine/x86_64-unix/winemac.so
  if command -v nm >/dev/null && ! nm -gU "$so" 2>/dev/null | grep -q ' _macdrv_functions$'; then
    echo "pin-check: $so has no DXMT table: rebuild it with winemac-dxmt_compat.c (README step 2)" >&2
    return 2
  fi
  k32_check "$w"
}
# A stock kernel32 runs most games, but UEVR's memory probes can hit a guard page that stock IsBadReadPtr lets
# through, and the game dies about 20 s after injection (README step 2, kernel32 with the guard-page fix). Warn, do
# not refuse. The build is not byte-reproducible, so this recognises Gcenx's stock 11.16 file, not our patched one.
k32_check() { # k32_check <wine dir>
  local w=$1 lib sys h hs stock=7faea9bc70292b3b
  lib=$w/lib/wine/x86_64-windows/kernel32.dll; sys=${WINEPREFIX:-}/drive_c/windows/system32/kernel32.dll
  [ -f "$lib" ] || return 0
  h=$(k32_sum "$lib") || return 0
  case $h in "$stock"*)
    echo "pin-check: $lib is Wine's stock kernel32, without the guard-page fix. Fine for most games; under UEVR a game can crash ~20 s after injection (README step 2)." >&2;;
  esac
  [ -f "$sys" ] || return 0
  hs=$(k32_sum "$sys") || return 0
  [ "$hs" = "$h" ] || echo "pin-check: the prefix's system32/kernel32.dll differs from Wine's own ($lib): copy one over the other, with wineserver down (README step 2)." >&2
}
k32_sum() { if command -v shasum >/dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi 2>/dev/null | cut -c1-64; }
