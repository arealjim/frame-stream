#!/bin/bash
# SPDX-License-Identifier: MIT
# build-opencomposite.sh [workdir] [wine-vulkan-1.dll] - OpenComposite (OpenVR -> OpenXR, GPL-3.0,
# https://gitlab.com/znixian/OpenOVR) cross-built on the Mac with Homebrew's mingw-w64, with opencomposite-mingw.patch:
#  - OpenVR 2.15.6 interfaces (IVRSystem_026, Compositor_029, Overlay_028, Input_011) that Half-Life: Alyx asks for;
#    the newer methods are stubs that report "not supported"
#  - MSVC's return convention for methods that return a struct (GetProjectionMatrix, GetEyeToHeadTransform, ...):
#    MSVC puts `this` first and the return slot second, mingw the other way round, so a mingw build hands the game
#    garbage matrices. The header splitter rewrites those methods with an explicit return pointer, as Wine's widl does
#  - the VRControlPanel export, IPCResourceManagerClient answered "not found", a log stream that exists before the
#    first static initialiser logs, and the mingw link and include fixes
# Needs Homebrew's mingw-w64, cmake, ninja and python3. The dll is the import library for Vulkan: mingw links
# against a dll directly, and Wine's own vulkan-1.dll is the one the game will load. A few minutes.
set -euo pipefail
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH   # Homebrew, also from a bare ssh where brew is not on PATH
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$PWD/opencomposite-build}; VK=${2:-$HERE/prefix/drive_c/windows/system32/vulkan-1.dll}
[ -f "$VK" ] || { echo "no vulkan-1.dll at $VK (pass the prefix's system32 copy)" >&2; exit 2; }
VK=$(cd "$(dirname "$VK")" && pwd)/$(basename "$VK")   # both absolute: used again after cd (review L1)
mkdir -p "$W"; W=$(cd "$W" && pwd); cd "$W"
PIN=cff07db75c4823afe93ed7027b03d5f7bc86f164
[ -d OpenOVR ] || git clone -q https://gitlab.com/znixian/OpenOVR.git OpenOVR
cd OpenOVR; git checkout -q -f $PIN; git clean -qfdx -e build
git submodule update --init -q libs/glm libs/openxr-sdk
git apply "$HERE/opencomposite-mingw.patch"
# Valve's 2.15.6 header, with one enum parameter widened to the integer the splitter can read
curl -sfL -o "$W/openvr-2.15.6.h.orig" https://raw.githubusercontent.com/ValveSoftware/openvr/v2.15.6/headers/openvr.h
echo "1e6ed57199896cc1f7c5484e50fa18955e97be15be690beb28d998c877ead7fd  $W/openvr-2.15.6.h.orig" | shasum -a 256 -c - >/dev/null \
  || { echo "openvr.h does not match the sha256 this was built with (2026-10-24)" >&2; exit 1; }
sed 's/ComputeDistortionSet( EVREye eEye, EVRDistortionChannel eChannel,/ComputeDistortionSet( EVREye eEye, uint32_t eChannel,/' "$W/openvr-2.15.6.h.orig" \
  > OpenVRHeaders/openvr-2.15.6.h
[ -d "$W/Vulkan-Headers" ] || git clone -q --depth 1 --branch v1.3.296 https://github.com/KhronosGroup/Vulkan-Headers.git "$W/Vulkan-Headers"
mkdir -p libs/vulkan/Lib; cp -R "$W/Vulkan-Headers/include" libs/vulkan/Include   # libs/vulkan is gitignored, so the clean above removed it
cp "$VK" libs/vulkan/Lib/vulkan-1.lib
rm -rf build
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/mingw.cmake" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-DWIN32 -DCMAKE_CXX_FLAGS="-DWIN32 -I$PWD/mingw-shim" \
  -DCMAKE_SHARED_LINKER_FLAGS=-static > "$W/cmake.log"   # -static: no mingw runtime dlls to ship beside it
nice -n 5 ninja -C build > "$W/build.log"
x86_64-w64-mingw32-strip -o "$W/openvr_api.dll" build/bin/vrclient_x64.dll
echo "OpenComposite built: $W/openvr_api.dll  (install with ./opencomposite.sh install <game-dir> $W/openvr_api.dll)"
