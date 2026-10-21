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
| `docs/re-notes.md` | notas de ingeniería inversa (un proyecto Ghidra iría en `re/`, ignorado) | Sí |
| `upstream/rePOP/` | submódulo: C generado + runtime (base de la Ruta B), no se edita | Sí (submódulo) |
| `port/vita/` | todo lo de la Vita: `src/` host, `compat/` POSIX/MMU, `hosttest/` ARM32 en Docker, `tools/vitaize.py`, `vendor/vitaGL/`, `livearea/` (la única; `art/` = imágenes fuente, ignorado) | Sí |
| `build.sh` | compila (`./build.sh`), `--hosttest`, `--vita3k` (instala el eboot y abre Vita3K) | Sí |
| `data_manifest.txt` | archivos que deben existir en `ux0:data/pop-sot/` | Sí |
| `converted/` | vídeos/audio convertidos para Vita | No |

## Datos del port

- Title ID Vita: `PSOT00001` · datos: `ux0:data/pop-sot/` · logs: `ux0:data/pop-sot/logs/log_NNN.log`
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
4. **`upstream/` no se edita:** las diferencias de la Vita van en `port/vita/` (vitaize.py copia y parchea en el build).
5. **Renderer:** vitaGL por defecto (`RECOMP_HW=1`): el rasterizador por software tarda minutos por fotograma
   en consola. Vita3K 0.2.1 (macOS) no muestra nada de vitaGL: ahí `RECOMP_HW=0` en `ux0:data/pop-sot/env.txt`.
   Las pruebas se hacen en la consola real, no en Vita3K (decisión del usuario, 2026-10-21).
6. **Un bug a la vez, guiado por log/dump.** Para crashes en consola: `vita-parse-core` sobre el
   `.psp2dmp` + `addr2line`, metodología de la skill `so-crash-triage`.
7. **Nada del disco al repo** (`.gitignore` anti-DMCA). Nunca subas ISO/XBE/datos/exports de Ghidra.
8. Lecturas masivas (pseudo-C de Ghidra, dumps) → resume primero (skill `psvita-token-economy`).

## Comandos

```bash
./build.sh                 # -> build/vita-port/pop-sot.vpk (VITASDK=~/vitasdk por defecto)
./build.sh --vita3k        # + instala el eboot en Vita3K y lo abre (instala el VPK la primera vez)
./build.sh --hosttest && port/vita/hosttest/run.sh run 900   # arranque en ARM32 (Docker) antes de la consola
VITA_IP=<ip> ../../tools/deploy.sh build/vita-port/eboot.bin PSOT00001  # consola (tras instalar el VPK)
python3 -I port/vita/tools/extract_game_data.py game_data_ntsc ftp://<ip>:1337 --movies   # datos + vídeos MP4 a la Vita (1,66 GB +
                           # movies/; --movies necesita ffmpeg;
                           # o una carpeta local en vez de ftp://). Sin bonus POP1/POP2 ni idiomas 1..4
python3 -I ../../tools/xbe_info.py game_data/default.xbe --md            # cabecera del XBE
python3 -I ../../tools/inventory.py game_data                      # inventario de datos
../../tools/media2mp4.sh game_data/VIDEO/30fps converted/movies  # vídeos -> MP4
```
