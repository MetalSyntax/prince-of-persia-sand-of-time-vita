#!/bin/bash
# Compila el port para psvita-toolkit (`psvita-toolkit build`) o a mano:
#   ./build.sh               VPK en build/vita-port/pop-sot.vpk (+ pop-sot.elf para `analyze`)
#   ./build.sh --hosttest    el mismo programa para Linux ARM32 (port/vita/hosttest/run.sh)
#   ./build.sh --vita3k      compila, instala el eboot en Vita3K (macOS) y lo abre como app normal
#                            (con foco: teclado/mando funcionan). Log: ux0:data/pop-sot/logs/log_NNN.log
set -e
cd "$(dirname "$0")"
if [ "$1" = "--hosttest" ]; then
    exec port/vita/hosttest/run.sh build
fi
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
cmake -S port/vita -B build/vita-port
make -C build/vita-port -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
# `psvita-toolkit analyze` busca el ELF con símbolos como build_dir/*.elf
cp build/vita-port/pop-sot build/vita-port/pop-sot.elf

if [ "$1" = "--vita3k" ]; then
    V3K="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0"
    TID=$(sed -n 's/.*"titleid": *"\([^"]*\)".*/\1/p' .psvita-toolkit.json)
    [ -d "$V3K/app/$TID" ] || { echo "Instala antes build/vita-port/pop-sot.vpk en Vita3K (arrástralo a la ventana)"; exit 1; }
    cp build/vita-port/eboot.bin "$V3K/app/$TID/eboot.bin"
    # cierre amable 5 s (con SIGTERM Vita3K "se cierra inesperadamente"); si un diálogo lo bloquea, -9
    osascript -e 'quit app "Vita3K"' >/dev/null 2>&1 & q=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x Vita3K >/dev/null || break; sleep 0.5; done
    kill "$q" 2>/dev/null; pkill -9 -x Vita3K; while pgrep -x Vita3K >/dev/null; do sleep 0.5; done
    open -a Vita3K --args -r "$TID"
    echo "Vita3K abierto con $TID; logs en $V3K/data/pop-sot/logs/"
fi
