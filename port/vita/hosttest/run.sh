#!/bin/sh
# Arranca el port en Linux ARM32 (Docker, contenedor armv7 emulado) con el
# mismo código que la Vita: MMU por software, vitaize, C generado, host Vita
# sobre hosttest/psp2_fake.c. Sirve para cazar crashes antes de la consola;
# no mide rendimiento (la CPU armv7 se emula).
#
#   port/vita/hosttest/run.sh build          compila (contenedor arm64 + cross gcc, ~15 min)
#   port/vita/hosttest/run.sh run [segundos] arranca el juego (por defecto 600 s)
#
# Logs, capturas (logs/frames/*.ppm) y saves: build/hosttest-data/.
# Variables útiles en run: HOSTTEST_PAD, HOSTTEST_FRAME_MS (ver psp2_fake.c), RECOMP_*,
# HOSTTEST_WINDOW=1 (ventana en vivo por XQuartz, teclado en window_sdl.c).
set -eu
PORT=$(cd "$(dirname "$0")/../../.." && pwd)
GAME=$(cd "$PORT/game_data_ntsc/" && pwd -P)
VITAINC="${VITASDK:-$HOME/vitasdk}/arm-vita-eabi/include"
DATA="$PORT/build/hosttest-data"
IMAGE=pop-sot-hosttest-cross
VOLUME=pop-sot-hosttest-build
RUNIMAGE=pop-sot-hosttest-run

case "${1:-}" in
build)
  docker build -q -t "$IMAGE" - <<'EOF' >/dev/null
FROM debian:trixie-slim
RUN dpkg --add-architecture armhf && apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-linux-gnueabihf libc6-dev-armhf-cross cmake make python3 libsdl2-dev:armhf libegl-dev:armhf libopengl-dev:armhf libgl-dev:armhf \
    && rm -rf /var/lib/apt/lists/*
EOF
  docker build -q --platform linux/arm/v7 -t "$RUNIMAGE" - <<'EOF' >/dev/null
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsdl2-2.0-0 \
    libegl1 libegl-mesa0 libopengl0 libgl1-mesa-dri && rm -rf /var/lib/apt/lists/*
EOF
  docker run --rm -v "$PORT":/port:ro -v "$VITAINC":/vitainc:ro -v "$VOLUME":/build "$IMAGE" sh -ec '
    mkdir -p /build/psp2inc
    cp -r /vitainc/psp2 /vitainc/psp2common /vitainc/vitasdk /build/psp2inc/
    cmake -S /port/port/vita -B /build/out -DHOSTTEST=ON -DPSP2_INCLUDE=/build/psp2inc \
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
      -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc >/build/configure.log
    make -k -C /build/out -j"$(nproc)" 2>&1 | grep -E "error|undefined|Linking|Built" || true
    test -x /build/out/pop-sot'
  ;;
run)
  mkdir -p "$DATA"
  # Ventana en vivo: XQuartz con "Permitir conexiones de clientes de red" y xhost +localhost.
  WINDOW=
  if [ -n "${HOSTTEST_WINDOW:-}" ]; then WINDOW="-e DISPLAY=host.docker.internal:0"; fi
  docker run --rm --platform linux/arm/v7 \
    -v "$VOLUME":/build:ro -v "$GAME":/game:ro -v "$DATA":/data \
    -e HOSTTEST_PAD -e HOSTTEST_FRAME_MS -e RECOMP_APU_TRACE -e RECOMP_TRAP_NULL -e RECOMP_HW \
    -e RECOMP_VITA_RT_FLIP -e EGL_PLATFORM=surfaceless -e LIBGL_ALWAYS_SOFTWARE=1 $WINDOW \
    "$RUNIMAGE" sh -c '
      for f in /game/*; do ln -sfn "$f" /data/; done
      cd /data && timeout '"${2:-600}"' /build/out/pop-sot; echo "salida: $?"'
  echo "logs: $DATA/logs"
  ;;
*)
  sed -n 2,12p "$0"; exit 2 ;;
esac
