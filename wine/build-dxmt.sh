#!/bin/bash
# SPDX-License-Identifier: MIT
# build-dxmt.sh [workdir] - DXMT (D3D11 -> Metal) at the commit oxrsys-winebridge pins, 64-bit release build, following
# DXMT's own ci.yml: LLVM 15.0.7 for x86_64 (DXMT links it to compile shaders), 3Shain's Wine headers, mingw, meson.
# Needs Xcode, Homebrew's mingw-w64, meson and ninja. 5.6 minutes from a fresh clone on an M5 MacBook Pro (2026-10-22), nearly all of it LLVM.
# v0.80 is too old for the bridge: its keyed-mutex record is 108 bytes and the bridge's 112.
set -euo pipefail
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH   # Homebrew, also from a bare ssh where brew is not on PATH
W=${1:-$PWD/dxmt-build}; mkdir -p "$W/dl"; W=$(cd "$W" && pwd); cd "$W"   # absolute: used again after cd (review L1)
PIN=4ddb20e54672c0cb56115ce80d6db1beef94ae28
[ -d dxmt ] || git clone -q https://github.com/3Shain/dxmt.git dxmt
cd dxmt; git checkout -q $PIN; git submodule update --init --recursive -q
cd "$W"
[ -d llvm-project ] || git clone -q --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git llvm-project
TC=$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/bin; SDK=$(xcrun --sdk macosx --show-sdk-path)
if [ ! -f llvm-darwin/lib/libLLVMCore.a ]; then
  cmake -B llvm-darwin-build -S llvm-project/llvm -DCMAKE_INSTALL_PREFIX="$W/llvm-darwin" -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DLLVM_HOST_TRIPLE=x86_64-apple-darwin -DLLVM_ENABLE_ASSERTIONS=On -DLLVM_ENABLE_ZSTD=Off -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=Off -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off \
    -DCMAKE_C_COMPILER="$TC/clang" -DCMAKE_CXX_COMPILER="$TC/clang++" -DCMAKE_OSX_SYSROOT="$SDK" -G Ninja > llvm-cmake.log
  nice -n 5 cmake --build llvm-darwin-build > llvm-build.log
  cmake --install llvm-darwin-build > /dev/null
fi
if [ ! -d wine-headers/include ]; then
  curl -fsSL https://github.com/3Shain/wine/releases/download/v8.16-3shain/wine.tar.gz -o dl/wine-3shain.tar.gz
  echo "289c7f19e270a3d3d0a6fdb07691b176c70a0795f6811e5255cba82425de4f10  dl/wine-3shain.tar.gz" | shasum -a 256 -c -   # review L8
  mkdir -p wine-headers; tar -xzf dl/wine-3shain.tar.gz -C wine-headers
fi
cd dxmt; rm -rf toolchains; mkdir toolchains; ln -s "$W/llvm-darwin" toolchains/llvm-darwin; ln -s "$W/wine-headers" toolchains/wine
# meson also wants a compiler for the build machine (the Mac); without one it stops at "not specified for build
# machine". Xcode's, with its SDK named, as for Wine.
cat > "$W/native-xcode.txt" <<NATIVE
[binaries]
c = ['$TC/clang', '-isysroot', '$SDK']
cpp = ['$TC/clang++', '-isysroot', '$SDK']
objc = ['$TC/clang', '-isysroot', '$SDK']
objcpp = ['$TC/clang++', '-isysroot', '$SDK']
NATIVE
rm -rf build
meson setup --cross-file build-win64.txt --native-file "$W/native-xcode.txt" -Dnative_llvm_path=toolchains/llvm-darwin -Dwine_install_path=toolchains/wine \
  build --buildtype release --prefix "$W/install64" --strip > "$W/meson64.log"
meson compile -C build >> "$W/meson64.log"; meson install -C build >> "$W/meson64.log"
echo "DXMT built into $W/install64:"; find "$W/install64" -name "*.dll" -o -name "*.so"
