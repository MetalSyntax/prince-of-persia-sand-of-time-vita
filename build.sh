#!/bin/bash
# Compila el port para psvita-toolkit (`psvita-toolkit build`) o a mano:
#   ./build.sh               VPK en build/vita-port/pop-sot.vpk (+ pop-sot.elf para `analyze`)
#   ./build.sh --hosttest    el mismo programa para Linux ARM32 (port/vita/hosttest/run.sh)
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
