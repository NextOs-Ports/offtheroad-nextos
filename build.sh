#!/bin/bash
# Off The Road 1.18.2 (engine xGen/Horde3D+bgfx sobre casca Cocos2d-x, arm64)
# so-loader -> NextOS Mali-450.  Toolchain/sysroot da versao ATUAL do NextOS.
set -e
TC=/mnt/ARQUIVOS/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain
CC=$TC/bin/aarch64-libreelec-linux-gnu-gcc
SR=$TC/aarch64-libreelec-linux-gnu/sysroot
export TMPDIR="/mnt/ARQUIVOS/TRABALHO CLAUDE CODE/99-TEMP-CLAUDE/claude-1000/gcc-tmp"
mkdir -p "$TMPDIR"

cd "$(dirname "$0")"
[ -x "$CC" ] || { echo "toolchain nao encontrado: $CC"; exit 1; }

SRCS="src/main.c src/so_util.c src/util.c src/error.c src/imports.c \
      src/jni_shim.c src/opensles_shim.c src/text_render.c \
      src/pthread_bridge.c src/egl_shim.c src/ot_bionic.c src/ot_setjmp.S"

$CC -O2 -g -fPIE -pie -fno-omit-frame-pointer -rdynamic -D_GNU_SOURCE \
    -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
    -Wno-int-conversion -Wno-incompatible-pointer-types \
    -o offtheroad $SRCS \
    -Isrc -I"$SR/usr/include" -I"$SR/usr/include/SDL2" \
    -I"$SR/usr/include/freetype2" \
    --sysroot="$SR" \
    -lSDL2 -lGLESv2 -lEGL -lfreetype -ldl -lm -lpthread -lstdc++

echo "BUILD OK -> $(file -b offtheroad | cut -d, -f1-3)"
