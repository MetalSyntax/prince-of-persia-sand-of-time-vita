# Prince of Persia: The Sands of Time → PS Vita (port nativo desde Xbox clásica)

Port por **recompilación nativa**, no emulación: el código del juego (decompilado o reconstruido) se compila
para el ARM de la Vita y el renderer Direct3D 8 se traduce a vitaGL/GXM. La XISO solo aporta **datos**.
Método y referencias: `../../README.md` y `~/MD/xbox-to-vita-ports.md` (THUG y Halo CE).

## Estructura

| Ruta | Qué es | ¿Se commitea? |
|---|---|---|
| `game_data/` | symlink a la XISO PAL extraída (solo lectura, nunca modificar) | No |
| `game_data_ntsc/` | symlink a la XISO **NTSC** extraída (disco de rePOP, referencia del port) | No |
| `docs/xbe.md`, `docs/inventory.md` | cabecera del XBE e inventario de datos generados por `new-port.sh` | Sí |
| `PORTING_PLAN.md` | plan por fases con puertas de decisión: **fuente de verdad del estado** | Sí |
| `re/` | notas de ingeniería inversa (`re/NOTES.md`); proyecto Ghidra y exports ignorados | Solo notas |
| `source/` | código del juego (decomp/reconstrucción), sin `#ifdef VITA` | Sí (si la licencia lo permite) |
| `port/<plat>/` | capa de plataforma: `vita/` (vitaGL), `desktop/` (SDL2+OpenGL de referencia) | Sí |
| `data_manifest.txt` | archivos que deben existir en `ux0:data/pop-sot/` | Sí |
| `converted/` | vídeos/audio convertidos para Vita | No |

## Datos del port

- Title ID Vita: `PSOT00001` · datos: `ux0:data/pop-sot/` · log: `ux0:data/pop-sot/pop-sot.log`
- Requisito en consola: `ur0:data/libshacccg.suprx` (ShaRKF00D), HENkaku/Enso, VitaShell.

## Hallazgos clave (detalle y evidencia en `PORTING_PLAN.md` §0)

- Disco Xbox **PAL** `US-029` v2, XDK 5558; motor **Jade**; vídeos Bink (`.int`), bigfile `prince.bf` (`BIG\0`).
- **Disco Xbox NTSC** (2026-10-19, `game_data_ntsc/`): `default.xbe` **idéntico** al de
  `upstream/rePOP/source/xbox-retail/target.h:10` (SHA-256 `753c58b1…ee379`, link 2003-10-20). Es el disco de
  referencia del port; el C generado de rePOP aplica tal cual. Ver `docs/xbe_ntsc.md` y `docs/inventory_ntsc.md`.
- **Base de código: `upstream/rePOP`** (submódulo, [FulGerNet/rePOP](https://github.com/FulGerNet/rePOP), rama `reSOT`): recomp estática de este juego ya jugable en PC/Android arm64. **Muy importante para el port**: es la base de la Ruta B. Brechas para Vita en `PORTING_PLAN.md` §0 (XBE NTSC≠PAL, 4 GB guest, MMIO por SIGSEGV, GPU Vulkan). No editar el submódulo; cambios propios en `port/vita/`. Actualizar con `git submodule update --remote upstream/rePOP`.
- No usar el código filtrado de Jade salvo decisión explícita del usuario.

## Reglas para el agente (IA)

1. **Trabaja una fase a la vez** según `PORTING_PLAN.md`. Al cerrar un paso, marca su checkbox y anota
   fecha + evidencia (comando, log, captura). No marques nada sin evidencia.
2. **Puertas de decisión (⛔ GATE):** si un gate falla, para y reporta al usuario con la evidencia. No
   improvises una ruta alternativa grande sin acuerdo.
3. **No asumas, confirma.** Afirmaciones sobre formatos, motor o funciones van con evidencia (offset,
   firma, función de Ghidra, hash). Lo no confirmado se marca *hipótesis*.
4. **`source/` no se ensucia:** diferencias de plataforma van en `port/<plat>/`. Nunca `#ifdef VITA` masivo.
5. **Renderer en dos pasos:** primero `port/desktop` (OpenGL) funcionando, luego Vita modelado sobre él.
6. **Un bug a la vez, guiado por log/dump.** Para crashes en consola: `vita-parse-core` sobre el
   `.psp2dmp` + `addr2line`, metodología de la skill `so-crash-triage`.
7. **Nada del disco al repo** (`.gitignore` anti-DMCA). Nunca subas ISO/XBE/datos/exports de Ghidra.
8. Lecturas masivas (pseudo-C de Ghidra, dumps) → resume primero (skill `psvita-token-economy`).

## Comandos

```bash
export VITASDK=$HOME/vitasdk PATH=$HOME/vitasdk/bin:$PATH
cmake -S port/vita -B build/vita && cmake --build build/vita     # -> build/vita/pop-sot.vpk
VITA_IP=<ip> ../../tools/deploy.sh build/vita/eboot.bin PSOT00001  # iteración rápida (tras instalar el VPK)
python3 -I ../../tools/xbe_info.py game_data/default.xbe --md            # cabecera del XBE
python3 -I ../../tools/inventory.py game_data                      # inventario de datos
../../tools/media2mp4.sh game_data/VIDEO/30fps converted/movies  # vídeos -> MP4
```
