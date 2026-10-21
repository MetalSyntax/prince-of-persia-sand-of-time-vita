# Plan de port: Prince of Persia: The Sands of Time (Xbox, 2003) → PS Vita

> Plan ejecutable por IA, paso a paso. Formato: `- [ ]` pendiente · `- [x] (AAAA-MM-DD) evidencia` hecho.
> ⛔ **GATE** = punto de decisión: si falla, la IA para y reporta al usuario con evidencia.
> Fuente de verdad del estado del port. Reglas de trabajo en `CLAUDE.md`.

## 0. Resumen honesto (2026-10-09)

**Veredicto:** hoy **no cumple** el requisito mínimo de la guía (`~/MD/xbox-to-vita-ports.md` §5.1): no hay fuente,
ni decompilación, ni reimplementación usable de este juego. Este plan es, en realidad, el plan de un **proyecto de
decompilación** con el port a Vita como meta final. Es un trabajo de años, no de semanas. La IA acelera la
decompilación función a función, pero no elimina la verificación ni el trabajo en hardware.

| Hecho | Evidencia |
|---|---|
| Nuestro disco: **PoP SoT Xbox PAL**, Title ID `5553001D (US-029)`, versión 2, retail, XDK **5558** | `docs/xbe.md` (SHA-256 `ef5d8a5a…acbc`) |
| `.text` ≈ 3,2 MB; librerías estáticas: D3D8, DSOUND, **Bink** (10+ secciones), D3DX8, XGRAPHICS | `docs/xbe.md` |
| Motor **Jade** (Ubisoft Montpellier/Montreal; el mismo de Beyond Good & Evil) | Wikipedia, RayDevWiki |
| Datos: `prince.bf` (348 MB, firma `BIG\0`, bigfile de Jade), audio en `SOUND/*.big+.fat`, vídeos Bink `.int` | `xxd`, `docs/inventory.md` |
| `dekamaru/openjade`: reimplementación **sin código publicado** (solo README + binarios) y declara SoT no soportado | GitHub |
| `theLlamaNet/poptools`: herramientas de modding de la trilogía (formatos), sin motor | GitHub |
| Filtración del código de Jade (dic-2022, build de *Rayman Raving Rabbids*) | RayDevWiki — ver ⚠ abajo |

> ⚠ **Código filtrado de Jade:** existe, pero es material propietario filtrado de **otro juego y otra versión del
> motor** (2006 vs 2003). Usarlo como fuente tiene riesgo legal; el port de THUG lo asumió con su propia filtración,
> pero es **decisión del usuario**. Por defecto este plan es **clean-room**: XBE propio + documentación pública de
> formatos. Si el usuario decide consultarlo, solo como referencia de nombres/estructuras en `re/`, nunca copiado a
> `source/`.

### Rutas

- **Ruta A (por defecto): decompilación incremental del XBE** con *delink + relink*, como `JSRF-Decompilation`:
  se separa el XBE en objetos, se reemplazan funciones por C una a una y se vuelve a enlazar un XBE que se prueba en
  **xemu** en cada paso. Al 100 % de C → build portable → Vita.
- **Ruta B (experimental): recompilación estática x86→C + HLE de las librerías XDK.** La XbSymbolDatabase identifica
  D3D8/DSOUND/Bink, que se reemplazan por implementaciones propias (D3D8→vitaGL, DSOUND→SceAudio, Bink→MP4).
  El resto del `.text` se traduce automáticamente a C. Es genérica: serviría para más juegos del boilerplate. Riesgos:
  saltos indirectos, x87, rendimiento en Cortex-A9. Solo con prototipo y GATE propio (Fase 3B).

> ⭐ **2026-10-19 — base de código encontrada: [FulGerNet/rePOP](https://github.com/FulGerNet/rePOP)** (rama `reSOT`,
> submódulo en `upstream/rePOP`). Es la **Ruta B ya hecha para este juego**: recomp estática con xboxrecomp (parcheado),
> C generado publicado en `source/xbox-{demo,retail}/gen` (87 + 111 MB), runtime con kernel/NV2A/USB/audio. Corre en
> Windows (D3D11/Vulkan) y tiene app **Android arm64 (Vulkan, SDL3)**: compila para AArch64 sin tocar el C generado
> (`-fwrapv -fsigned-char -ffp-contract=off -fno-strict-aliasing`). Licencia MIT solo para el código propio
> (`docs/LICENSING.md`). Estado upstream: jugable en primeros niveles, validación en curso.
>
> Lo que falta para Vita (confirmado leyendo `docs/RECOMP.md` §Android host y el runtime):
> 1. **Versión del XBE:** su retail es **NTSC, link 2003-10-20**; el nuestro es **PAL v2, link 2004-01-12** (mismo
>    Title ID `5553001D`). Las direcciones no coinciden → conseguir el XBE NTSC o regenerar con su pipeline sobre el PAL.
> 2. **Espacio de direcciones:** reserva los 4 GB de guest (`xbox_memory_layout.c`); en ARM32 no cabe. A favor:
>    todo acceso pasa por `XBOX_PTR(addr) = addr + g_xbox_mem_offset` (`recomp_types.h:413`) → remapear rangos.
> 3. **MMIO por SIGSEGV:** APU/AC'97/OHCI se atrapan con fallos de página; la Vita no tiene señales → chequeo explícito.
> 4. **GPU:** backend nativo Vulkan/D3D11 → nuevo backend NV2A→vitaGL/GXM.
> 5. **Recursos:** 512 MB RAM y Cortex-A9 vs Adreno 750 (que ya sufre caídas de FPS en upstream). Medir pronto.
>
> Implicación: la Ruta A (decomp manual) pasa a respaldo; la Fase 3B se reorienta a **portar el runtime de rePOP a
> Vita**. Pendiente de replanificar con el usuario.
>
> **Decisión (2026-10-19):** el usuario consigue una copia **NTSC** para usar el C generado de rePOP tal cual.
> - [x] (2026-10-19) `~/Downloads/Prince of Persia - The Sands of Time (USA).iso` (XISO plano, 2 500 657 152 B):
>       `default.xbe` 4 603 904 B, Title ID `0x5553001D`, región NA+JP, link 2003-10-20, SHA-256
>       `753c58b1…1ee379` = **coincide** con `upstream/rePOP/source/xbox-retail/target.h:22`. Leído directo del ISO
>       sin extraer. Opcional: demo E3 2003 = `79C1A478…D4F5`.
> - [x] (2026-10-19) XISO NTSC montada aparte del PAL: extraída con `extract-xiso -x` desde
>       `/Volumes/TOSHIBA EXT/Xbox/Prince of Persia - The Sands of Time (USA).iso` (2 499 878 784 B, SHA-256 ISO
>       `e90439f4…3262` — **difiere** del ISO de `~/Downloads`, es un rip distinto) a
>       `/Volumes/TOSHIBA EXT/Xbox/iso-extracted/Prince of Persia - The Sands of Time (USA)/`,
>       symlink `game_data_ntsc/`. El `default.xbe` extraído (4 603 904 B, región NA+JP, versión 42753, link
>       2003-10-20) tiene SHA-256 `753c58b1…ee379` = **coincide** con `target.h:10` de rePOP; descrito en
>       `docs/xbe_ntsc.md`, inventario en `docs/inventory_ntsc.md` (2,3 GB; MP4 desde `VIDEO/`, 84 `.int` NTSC).
> - [ ] Probar el arranque del disco NTSC en xemu como referencia (difiere del rip de `~/Downloads`).

---

## 1. Datos en la Vita

Ruta: `ux0:data/pop-sot/`, con la estructura de la XISO. Copiar por **USB**.

| Se copia | Se omite |
|---|---|
| `default.xbe`, `prince.bf`, `shaders.bin`, `MEDIA/`, `SOUND/` | `_resources/` (arte de dashboard, no es del disco original) |
| Vídeos convertidos a MP4 del set `VIDEO/30fps/` (94 archivos) | `VIDEO/25fps/` (6 duplicados PAL50) y los `.int` originales (2,2 GB) |
| `POP1/` solo si se porta el PoP clásico incluido (no es el objetivo) | `default.tbn`, `fanart.jpg` |

Estimado en `ux0:`: **≈ 0,9 GB + MP4** (confirmar en 2.4). Archivos mínimos: `data_manifest.txt`.

---

## Fase 0 — Setup y smoke en consola

- [x] (2026-10-09) Port creado con `new-port.sh`; `docs/xbe.md` y `docs/inventory.md` generados.
- [x] (2026-10-09) `data_manifest.txt` escrito y verificado contra `game_data/` (6/6 rutas existen).
- [x] (2026-10-09) Smoke VPK compila: `build/vita/pop-sot.vpk`.
- [ ] Instalar el VPK, copiar los archivos del manifest por USB a `ux0:data/pop-sot/` → **pantalla verde**.
      Guardar el log en `logs/`.
- [ ] Instalar **xemu** en el Mac y confirmar que el disco arranca y se juega (referencia para comparar en todo
      el proyecto). Requiere BIOS/MCPX/HDD propios del usuario.

## Fase 1 — Reconocimiento del XBE

- [ ] 1.1 Ghidra + `XboxDev/ghidra-xbe`; importar `game_data/default.xbe` en `re/ghidra_project/` (ignorado).
- [ ] 1.2 Aplicar la XbSymbolDatabase y marcar las librerías XDK. Contar las **funciones de juego** reales
      → `re/NOTES.md`.
- [ ] 1.3 Mapa del motor por evidencia: rutas de assert/`__FILE__`, strings de módulos Jade (`AI`, `ENG`, `GDK`,
      `SND`, `BIG`…), RTTI si existe. Tabla módulo → nº de funciones → rango de direcciones.
      *Prompt IA:* "Agrupa las funciones de juego por las strings `.c`/`.cpp` que referencian. Propón el árbol de
      fuentes original (estilo `fable-source-tree`) en `re/source_tree.md`, marcando cada entrada como
      confirmada o hipótesis."
- [ ] 1.4 Revisar la documentación pública: `hogsy/jaded`, RayDevWiki (Jade), `poptools`, foros princed.org.
      Lo aplicable a SoT va a `re/NOTES.md` con su enlace.
- [ ] ⛔ **GATE 1 — tamaño del proyecto:** con nº de funciones de juego × ritmo medido (Fase 3, 50 funciones de
      prueba) → estimación en meses. Presentar al usuario para decidir si se sigue, se pausa o se prueba la Ruta B.

## Fase 2 — Formatos de datos y herramientas

- [ ] 2.1 `tools/jade_bf.py`: parser del bigfile `prince.bf` (cabecera `BIG\0`, tabla de archivos, directorios).
      Listar todas las entradas con tipo y tamaño. Self-check con asserts. Referencia: `poptools` y `jaded`.
- [ ] 2.2 Parser de `SOUND/*.fat` + `*.big` (índice + datos). Identificar el codec (XADPCM/PCM/WMA) y probar la
      conversión de 10 sonidos.
- [ ] 2.3 Texturas dentro del `.bf`: formato Xbox (DXT o swizzle NV2A), unswizzle y exportación PNG de prueba.
- [ ] 2.4 Vídeos: `tools/media2mp4.sh game_data/VIDEO/30fps converted/movies`. Medir el tamaño total y actualizar §1.
- [ ] 2.5 `shaders.bin`: identificar el formato (vertex/pixel shaders NV2A precompilados). Documentar cuántos hay y
      de qué tipo; serán la lista de trabajo del renderer.

## Fase 3 — Decompilación incremental (Ruta A)

**Base técnica:** delink del XBE en objetos por módulo (tabla de 1.3), reemplazo función a función con
verificación en xemu.

- [ ] 3.1 Montar el pipeline *delink → compilar C → relink → XBE* (ver `JSRF-Decompilation`: delink de
      `default.xbe` y reempaquetado con `cxbe` de nxdk). Primer hito: XBE re-enlazado **sin cambios** que arranca en
      xemu.
- [ ] 3.2 Toolchain: clang con ABI MSVC (`-target i386-pc-windows-msvc`), igual que el port de Halo para el
      código del juego. Headers: tipos Jade reconstruidos en `source/include/`.
- [ ] 3.3 Bucle por función (la IA lo ejecuta en lote, el humano valida en xemu por hitos):
      1. Elegir una función hoja (sin llamadas no resueltas) del módulo actual.
      2. Pseudo-C de Ghidra → C limpio con nombres/tipos. Resumir antes si es largo (`psvita-token-economy`).
      3. Compilar y reemplazar en el XBE. Prueba automática: arranque + escena de prueba en xemu sin crash.
      4. Registrar en `re/progress.csv` (dirección, nombre, módulo, estado).
- [ ] 3.4 Orden de módulos: utilidades/matemáticas → BIG/IO → recursos → render → mundo → IA → gameplay → UI.
- [ ] 3.5 Medir el ritmo real con las primeras 50 funciones → alimentar el GATE 1.

### Fase 3B — Prototipo de recompilación estática

> ⚠ Obsoleta (2026-10-19): sustituida por la **Fase 3R** (rePOP ya es esta recompilación, hecha).
> Los antiguos 3B.1–3B.2 y el GATE 3B quedan en el historial de git.

### Fase 3R — Port del runtime de rePOP (`upstream/rePOP`) a la Vita

**Decisión del usuario (2026-10-19):** avanzar la Ruta B con rePOP: usar el C generado de
`upstream/rePOP/source/xbox-retail/gen` (XBE NTSC verificado) y portar el runtime (el mismo que compila para
Android arm64 vía `port/android/CMakeLists.txt`) a la Vita. El submódulo no se edita; todo va en el propio port.

**Modelo:** espejar `port/android/` de rePOP, con estos huecos a cerrar (todos confirmados en `docs/RECOMP.md`
§Android y en el código):

1. **GPU:** el encoder nativo (SDL3-GPU/Vulkan, HLSL→SPIR-V con dxc) no existe en Vita → backend propio
   NV2A→vitaGL/GXM. El estado NV2A, el executor de pushbuffer y el intérprete VSH se comparten; el encoder
   es lo que se escribe (`nv2a_hw_native.inc` queda excluido). Vertex programs de NV2A → shaders GLSL ES;
   register combiners → shaders o estado fijo; texturas por el decodificador software ya existente.
2. **Memoria/espacio de direcciones:** sin `mmap`/VMA en Vita (newlib/SceKernel). La RAM real de Xbox es 64 MB
   → heap-backed con desplazamiento global (recomp_types.h:413 `XBOX_PTR(addr) = addr + g_xbox_mem_offset`)
   y rangos MMIO a direcciones fuera del heap. No se reserva nada con PROT_NONE.
3. **MMIO por señales:** `apu_mmio_hook.c`, `ohci.c` y la trampa AC'97 usan SIGSEGV + decode de instrucción;
   en Vita se convierte a **chequeo explícito de rangos** en los accessors (misma lógica, sin señales).
4. **Hilos/sincronización:** pthread de Vita (ScePthread) desde el SDK; 60 Hz de vblank con SceKernelTimer,
   1 warn del runtimes: pilas de hilos guest más pequeñas (Android usa 8 MB por defecto — no cabe).
5. **Archivos:** `ux0:data/pop-sot/` con nombres case-insensitive (`kernel_path.c` de Android es el patrón);
   sin `memfd_create` → memoria normal; paths con espacios y `ux0:` OK.

> Corrección (2026-10-19): el punto 2 tal como estaba ("heap-backed con desplazamiento global") no basta: la
> ventana contigua (`0x80000000`) y la RAM están a 2 GB de distancia y no caben con un solo desplazamiento en
> la VA de usuario de la Vita. El punto 4 también: con *emutls* el coste de TLS existe pero es acotado.

**Diseño aplicado (2026-10-19), verificado leyendo el código de upstream:**

- **Memoria — MMU por software (`port/vita/compat/vvm.[ch]`).** `host = H + va` (32 bits con vuelta) se
  resuelve por tabla de páginas de 4 KB: `vvm_ptr(f) = f + vvm_pt[f >> 12]` (tabla de 4 MB). Un **bloque
  real de 112 MB** cubre de una vez NV2A/thunks/MCPX/flash/RAM (guest `0xFD000000..0x04000000` con la
  vuelta) con desplazamiento 0: todo `H + va` del runtime en esas zonas es memoria real sin cambios. La
  ventana contigua (64 MB, almacenamiento aparte **igual que upstream**, `xbox_memory_layout.c:2511`), la
  apertura tiled (alias de la contigua), los espejos de RAM y las reservas `EXT_VMA` van por tabla.
  `vvm.c` implementa `VirtualAlloc/Free/Protect` y `CreateFileMapping/MapViewOfFileEx`; las de
  `win32_compat.c` se renombran con `-D`.
- **`xbox_GetMemoryOffset()`** devuelve en Vita `contig_real - 0x80000000`: el ejecutor NV2A, OHCI y el
  trazador hacen `mem + va` con `va` en la ventana contigua (`nv2a_pb_exec.c:120`, `dma_resolve`).
- **`port/vita/tools/vitaize.py`** (corre en configure; falla si falta un ancla): copia a
  `build/<dir>/vitaize/` el `recomp_types.h` del C generado con `XBOX_PTR()` por `vvm_ptr()` (pre-incluido
  solo en `gen/`) y reescribe los accesos crudos `(uintptr_t)X + g_*_offset` del runtime (≈70) por
  `vvm_ptr()`, más anclas: página de kernel falsa, `guest_probe.h`, `muldiv64` sin `__int128` (self-check
  contra enteros de Python, 100 000 casos) y `s_hw` del backend GPU ausente.
- **Ramas `__ANDROID__` del runtime** = su camino POSIX completo (particiones emuladas que el retail exige
  para no volver al dashboard, rutas sin mayúsculas, pilas de 8 MB): activas en `win32_compat`,
  `kernel_path/file/bridge`, `guest_vmem`, `nv2a_pb_scan`, `target_hooks`. Fuera: el layout (reservaría
  4 GB) y `nv2a_pb_exec` (traería el encoder SDL/Vulkan).
- **Registros guest `__thread`:** el GCC de VitaSDK usa *emutls*; GCC lo factoriza (≈3 200 llamadas a
  `__emutls_get_address` en 74 000 instrucciones de `engine_0000.c`). Optimización pendiente (TPIDRURO).
- **Host Vita (`port/vita/src`):** `main_vita.c` (espejo de `main_android.c`: SHA-256 del XBE, logs en
  `ux0:data/pop-sot/logs/`), `fb_vita.c` (framebuffer guest → `sceDisplay` 725×544, sin vitaGL),
  `input_vita.c` (SceCtrl + táctil trasero para Black/White), `audio_vita.c` (xa2_* → SceAudio). De
  upstream tal cual: `options_android.c`, `host_files.c`, `sha256.c`, `scene.c`.
- Render V0: `RECOMP_HW=0` → rasterizador por software del ejecutor NV2A sobre la memoria guest.
- Lo que dejó la sesión anterior sin commitear (fork de 3 758 líneas de `xbox_memory_layout.c`, script que
  apuntaba a un `.patch` inexistente, `qemu_shim.h` copiado que nunca se habría usado) está en
  `port/vita/attic/` sin borrar.

Fases concretas:

- [ ] V0.1 `port/vita` del port (no del template): CMake que compila el **runtime rePOP** (kernel/audio/apu/
      usb/video + gen de retail) para ARM Vita con los flags guest (`-fwrapv -fsigned-char -ffp-contract=off
      -fno-strict-aliasing`), sin GPU (stub), con host propio mínimo (main/input/audio/archivos).
      Hito: **compila** (link completo), delegando en stubs lo GPU.
      - [x] (2026-10-19) `cmake -S port/vita -B build/vita-port && make -C build/vita-port -j10` → link
            completo sin símbolos sin resolver: `build/vita-port/pop-sot.vpk` 13 463 422 B (SHA-256
            `5619b48b…`), ELF `.text` 26,1 MB / `.bss` 20,7 MB, heap newlib 256 MB, SFO `ATTRIBUTE2=12`.
            Compilación completa ≈16 min a -j10 (M4). Solo compila: **no** probado en consola.
- [x] (2026-10-20) **Prueba previa en ARM32 sin consola (`port/vita/hosttest/`)**: el mismo programa
      (vvm, vitaize, C generado, host Vita) compilado para Linux armv7 (`-DHOSTTEST=ON`, psp2 falso en
      `hosttest/psp2_fake.c`) y ejecutado en Docker `linux/arm/v7`: `port/vita/hosttest/run.sh build|run`.
      Capturas PPM en `build/hosttest-data/logs/frames/`, crash → `[CRASH] pc/lr` + registros guest.
      No mide rendimiento (CPU emulada). Bugs encontrados y corregidos con él:
      1. `guest_vmem_free` limpiaba páginas con `H + va` crudo → `vitaize` (crash `memset` en `0xB9765000`).
      2. `scene.c` (logs de Jade) leía `base + va` crudo → `vitaize` (crash `read_name`).
      3. Páginas EXT_VMA comprometidas por separado no eran contiguas en el host → arena lineal en
         `vvm.c` (`VVM_EXT_ARENA_MB`, 64 en Vita; pico medido 18,5 MB hasta el menú).
      4. Sin APU el motor crashea tras cargar (documentado en `xbox_memory_layout.c`) → APU emulada con
         accesores MMIO en el DirectSound generado (`compat/mmio_vita.[ch]`, `trap_mmio` en vitaize,
         5 345 accesos) + `RECOMP_AC97_READY=1`, `RECOMP_APU_DSP_ACK=gp+0x810` como Android.
      5. Overlay PVIDEO (vídeos Bink) solo existía en `fb_present.c` → portado a `fb_vita.c`; sin él, el
         D3D del juego espera en bucle a que `BUFFER` vuelva a 0 tras cada vídeo.
      6. Pilas: `pthread_create` sin atributos da 32 KB en VitaSDK → `_pthread_stack_default_user` 1 MB;
         mínimo de `CreateThread` 8 MB (Android) → 1 MB (marco máx. medido 4 KB en Thumb-2).
      Resultado (15 min, `run 900`): sin crash; intro Ubisoft y logo POP TEAM visibles con colores
      correctos, mundos `MenuBoot` → `menu3D` + Prince cargados; el menú 3D se rasteriza (1 813 draws,
      156 K triángulos) pero a decenas de s por cuadro bajo emulación.
- [ ] V0.2 Prueba de kernel: hilos guest, vblank 60 Hz por timer, `KeSetEvent` → el juego llega al checking de
      discs / dibuja algo por software en un framebuffer → blit a la pantalla.
      Hito: **cartel/logo o pantalla del juego visible** en la consola.
      Implementado sin verificar: hilos por `CreateThread`→pthread del runtime, vblank de upstream
      (`RECOMP_VBLANK=1`), `fb_vita.c` (sceDisplay, sin vitaGL), rasterizador software (`RECOMP_HW=0`).
- [ ] V0.3 Audio (2026-10-20: APU emulada activa por accesores MMIO; la primera escritura APU llega
      al modelo en hosttest. Falta oírlo en consola): `dsound_device`/WMA → SceAudio (decodificar WMA es igual que en Android).
      `audio_vita.c` (xa2_* → SceAudio) hecho, pero la APU emulada de upstream depende de trampas de página
      (AC'97/APU) que en Vita no saltan (`VirtualProtect` es no-op): **sin sonido** hasta resolverlo
      (opción: `kuKernelRegisterExceptionHandler` de kubridge, o accesores explícitos en `vvm_ptr`).
- [ ] V0.4 Input: SceCtrl con el mapeo del pad de Xbox (✕/◯ invertidos respecto a A/B de Xbox: X=A, ◯=B,
      □=X(sin espada), △=Y, R1=trig. der., L1=trig. izq./rebobinar, Select=BACK, Start=START).
      `input_vita.c` hecho con ese mapeo + táctil trasero izq./der. = Black/White; sin verificar.
- [ ] ⛔ **GATE 3R.1:** el juego arranca en la consola hasta el menú (aun a 1–5 fps, con el rasterizador
      software de respaldo o un framebuffer parcial) sin crashear y con logs legibles.
- [x] (2026-10-20) **V1.1 + V1.2 backend GPU sobre vitaGL** (`port/vita/src/nv2a_hw_vita.inc`): la interfaz
      `ng_*` del encoder de Android sobre vitaGL, incluida por la copia vitaizada de `nv2a_hw_d3d11.inc`
      (estado NV2A, superficies, decodificador de texturas y despacho compartidos). Combiners: un fragment
      shader **generado por estado** (control words resueltos; el uber shader de upstream sería caro en la
      GPU de la Vita). Vertex programs **en la GPU**: cada programa se traduce una vez con
      `nv2a_vsh_emit_hlsl` de upstream (ajustes HLSL→CG/GLSL en `vg_fix_hlsl`). Vídeos Bink por el
      overlay compuesto en GL. CG en la Vita, GLSL 1.20 en hosttest (Mesa llvmpipe, `RESOT_HW_GLSL`).
      vitaGL **vendorizado** (`port/vita/vendor/vitaGL`, árbol de Skater Nation; `NO_DEBUG=1
      HAVE_SHADER_CACHE=1`). Evidencia hosttest (2 min, saltando vídeos con `HOSTTEST_PAD`): intro y logos,
      pantalla legal, y **menú principal 3D renderizado** (jungla con niebla, texturas, «Start New Game…»)
      a ~260 draws/cuadro; sin vertex programs en GPU el menú no llegaba a hacer flips.
      Pendiente en consola: orientación de los render targets (`RECOMP_VITA_RT_FLIP` en
      `ux0:data/pop-sot/env.txt`), memoria de vitaGL, rendimiento. Huecos: sin color de mezcla
      constante (vitaGL no tiene glBlendColor), visibilidad aproximada, sin Hor+ (4:3).
- [ ] V1.1 Backend GPU sobre vitaGL: empezar por el **menú** (2D simple) y luego el primer nivel.
- [ ] V1.2 Rendimiento: objetivo ≥ 15–30 fps en el primer nivel; caché de shaders en `ux0:data/pop-sot/`.
- [ ] ⛔ **GATE 3R.2:** jugabilidad estable (menú + primer nivel a 30 fps) antes de pulir audio/vídeo finales.

### Fases 4–6 (Ruta A, respaldo; conservadas del plan original)

## Fase 4 — Build portable en PC (referencia)

Requiere la Fase 3 al 100 % (o 3B validada).

- [ ] 4.1 `port/desktop`: SDL2 + OpenGL; D3D8 → GL siguiendo el patrón de `halo-ce-universal`; DSOUND → SDL audio;
      Bink → MP4 o saltar.
- [ ] 4.2 Hitos con captura: menú → primer nivel (palacio de Azad) → Daga del Tiempo (rebobinado) → combate →
      guardado en un punto de guardado.
- [ ] ⛔ **GATE 4:** el primer acto es jugable en escritorio con datos Xbox, sin crashes.

## Fase 5 — Vita (Ruta A)

- [ ] 5.1 `port/vita`: archivos `ux0:data/pop-sot/`, input (controles: ✕ saltar, □ ataque, ◯ rodar/soltar,
      △ acción, R1 bloquear/pared, L1 rebobinar, táctil trasero para el poder secundario), audio SceAudio,
      vídeo SceAvPlayer.
- [ ] 5.2 Renderer Vita sobre el GL de referencia. Atención al **efecto de arena/brillo** y a los post-procesos.
- [ ] 5.3 El **rebobinado** guarda un buffer histórico del estado: medir el coste en RAM y CPU en la Vita.
- [ ] 5.4 Primer arranque en consola; crashes con `vita-parse-core` + `so-crash-triage`.

## Fase 6 — Rendimiento, QA y empaquetado

- [ ] 6.1 Objetivo 30 fps (el original va a 30 en Xbox). Resolución 75 %, LOD y partículas configurables.
- [ ] 6.2 Panel `Select+Start`, `settings.txt`, saves en `ux0:data/pop-sot/save/`.
- [ ] 6.3 Partida completa de prueba y guía de instalación USB.

---

## Riesgos

| Riesgo | Mitigación |
|---|---|
| No hay base de código: la decompilación completa lleva años | GATE 1 con estimación medida; Ruta B como alternativa |
| Tentación de usar el código filtrado de Jade | Clean-room por defecto; decisión explícita del usuario |
| OpenJade es cerrado | No se depende de él |
| Verificación sin hardware Xbox | xemu como referencia + capturas comparadas |
| Bink integrado en el motor | Bink → MP4 con salto limpio (método Halo) |
| Rebobinado = carga extra de RAM/CPU | Medición temprana (5.3) |

## Fuentes
- https://github.com/dekamaru/openjade · https://github.com/theLlamaNet/poptools
- https://raydev.wiki/jade:overview (motor Jade y filtración de 2022)
- https://github.com/XboxDev/ghidra-xbe · https://codeberg.org/xen486/JSRF-Decompilation
- `~/MD/xbox-to-vita-ports.md` (método THUG/Halo)
