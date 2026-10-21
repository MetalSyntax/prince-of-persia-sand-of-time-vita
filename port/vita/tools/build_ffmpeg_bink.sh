#!/bin/bash
# FFmpeg mínimo para la Vita: solo lo necesario para reproducir los Bink originales
# del disco (VIDEO/*.int) en la consola: demuxer bink + decodificadores bink,
# binkaudio_dct y binkaudio_rdft. Lo usa src/movie_vita.c. El libavcodec de
# VitaSDK no trae Bink. Salida en build/ffmpeg-bink/{include,lib} (fuera de git).
# Uso: port/vita/tools/build_ffmpeg_bink.sh   (no hace nada si ya está compilado)
set -euo pipefail
cd "$(dirname "$0")/../../.."
VER=7.1.1
SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
SRC=build/ffmpeg-src
OUT="$PWD/build/ffmpeg-bink"
[ -f "$OUT/lib/libavcodec.a" ] && exit 0
export VITASDK="${VITASDK:-$HOME/vitasdk}" PATH="${VITASDK:-$HOME/vitasdk}/bin:$PATH"
mkdir -p "$SRC"
tarball="$SRC/ffmpeg-$VER.tar.xz"
[ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
echo "$SHA  $tarball" | shasum -a 256 -c - >/dev/null || { echo "SHA-256 de $tarball no coincide" >&2; exit 1; }
[ -d "$SRC/ffmpeg-$VER" ] || tar -xf "$tarball" -C "$SRC"
cd "$SRC/ffmpeg-$VER"
./configure --prefix="$OUT" --enable-cross-compile --cross-prefix=arm-vita-eabi- \
  --target-os=none --arch=arm --cpu=cortex-a9 --enable-neon \
  --extra-cflags="-O3 -mfloat-abi=softfp -mfpu=neon" \
  --disable-everything --disable-programs --disable-doc --disable-debug --disable-network \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-pthreads \
  --disable-runtime-cpudetect --enable-static --disable-shared \
  --enable-demuxer=bink --enable-decoder=bink --enable-decoder=binkaudio_dct \
  --enable-decoder=binkaudio_rdft --enable-protocol=file
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
make install
