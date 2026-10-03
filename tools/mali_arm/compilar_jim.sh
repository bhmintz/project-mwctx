#!/bin/sh
# Builds JimVulkan (PanVK) from out/jim/mesa with the NDK in out/jim/android-ndk-r29, both on D: (WSL's own
# disk lives on C:, which is nearly full). Run inside WSL from the repo root.
# The first stage needs LLVM 18 (the one with libclc and the SPIR-V translator installed); Ubuntu's default
# llvm-config is 21, so it is forced with a native file before build.sh configures it.
set -eu
cd "$(dirname "$0")/../../out/jim/mesa"
if [ ! -f build-host/build.ninja ]; then
   printf "[binaries]\nllvm-config = '/usr/bin/llvm-config-18'\n" > llvm18.ini
   meson setup build-host --native-file llvm18.ini \
      -Dbuildtype=debugoptimized \
      -Dmesa-clc=enabled -Dprecomp-compiler=enabled \
      -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dllvm=enabled \
      -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
      -Dtools= -Dvideo-codecs= \
      -Dallow-fallback-for=libdrm --force-fallback-for=libdrm,expat,zlib \
      -Dlibdrm:default_library=static
fi
./build.sh "$PWD/../android-ndk-r29"
