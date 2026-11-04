# Plan de rendimiento en consola: un cuello de botella a la vez

Documento vivo. Cada cuello tiene **estado**, **evidencia**, **siguiente paso** y **cómo se mide**.
Al terminar una prueba en la Vita: añadir una fila en el [Registro de pruebas](#registro-de-pruebas)
y actualizar el estado del cuello. Los planes anteriores (`OPTIMIZATION_PLAN_VITA.md`,
`pop_sot_vita_optimization_plan.md`) quedan como referencia; este es el que se sigue.

Estados: ⬜ pendiente · 🔬 en prueba · ✅ resuelto · ❌ descartado (con la medida que lo descartó)

## Punto de partida (menú principal, consola, 2026-10-26)

| Log | fps | Ejecutor ocupado / 10 s | Juego esperando KickOff / 10 s | compose + present |
|---|---|---|---|---|
| log_031 | 2,5 | 9,4 s | 6,8 s | 11,8 + 0,2 ms |
| log_032 | 4,9 | 8,3 s | 5,2 s | 12,7 + 0,2 ms |
| log_033 | 4,9 | 7,3 s | 3,6 s | 0,2 + 0,2 ms |

Conclusiones que guían el plan:

- **La GPU de la Vita no es el cuello.** `compose` y `present` suman menos de 13 ms. Por eso
  `RECOMP_RENDER_PERCENT=50` no cambia los fps.
- **Manda la CPU.** El ejecutor NV2A (hilo de ack) está ocupado el 73–94% del tiempo y el juego
  pasa un tercio esperándolo.
- **Vita3K no sirve para medir esto:** ejecuta el ARM a la velocidad del Mac y no simula la CDRAM
  sin caché (por eso allí da unos 20 fps).
- Si el ejecutor fuera instantáneo, el hilo del juego (63% de un núcleo ≈ 126 ms por fotograma)
  daría un tope de unos 8 fps. Hay que atacar los dos.

## Cómo medir (líneas del log, cada 5–10 s)

- `[HW vita] X fps; ... ms/frame: compose A, present B, draws C (texturas D)`: C es el tiempo
  dentro de `ng_draw_program_batch` (incluye D, que es `vg_texture`). Existe desde 2026-10-30.
- `[KICK] over 10 s: ... waited W ms ...; executor ... E ms executing`: E/10 s es la ocupación del
  ejecutor. **Lo que E tiene de más sobre C × fotogramas** es la lectura del pushbuffer y el
  despacho de métodos (upstream).
- `[THR] ... cpu%=`: % de un núcleo por hilo. Solo vale la línea con 10 s exactos desde la anterior.
- `[MEM] cada 10 s`: RAM, CDRAM y heap libres. Vigilar tras cada cambio de memoria.

Probar siempre en el mismo sitio (menú principal, sin tocar nada, unos 60 s) y comparar a partir
del segundo 50.

---

## C1. Ventana contigua del Xbox en CDRAM sin caché — ❌ la ventana entera en RAM no cabe; quedan alternativas

**Evidencia.** `compat/vvm.c` reserva los 64 MB de memoria contigua del Xbox (pushbuffer, vertex
buffers, texturas: direcciones `0x80xxxxxx`) como `USER_CDRAM_RW`. Para la CPU de la Vita la CDRAM
no tiene caché. De ahí leen el ejecutor (cada palabra del pushbuffer, unos 100 000 métodos en el
menú), el hash de texturas (128 lecturas × 134 texturas por fotograma), el hash de la caché de
VBO (9 × entradas por draw) y el propio juego.

**Intento 1 (2026-10-30): los 64 MB en RAM — ❌.** Con `RECOMP_VITA_CONTIG_RAM=1` (y heap de 56 a 40 MB,
arena EXT_VMA de 32 a 24 MB) quedan 40 MB de RAM tras el layout. La arena se lleva 24 y
`ng_init` no consigue los 12,4 MB de búferes de vértices: `[HW vita] sin memoria para buffers de
vértices: rasterizador por software` → pantalla negra a 0 fps (log_037). Upstream crea la ventana
como una asignación propia de 64 MB (no es un alias de la RAM guest), así que no se puede compartir.
- Por defecto vuelve a CDRAM. El flag sigue disponible en `env.txt` para pruebas.
- Se conservan el heap de 40 MB y la arena de 24 MB: con la ventana en CDRAM dejan a vitaGL unos
  24 MB más de RAM.

**Alternativas (decidir con los números de C2):**
1. **Copia en bloque del segmento de pushbuffer** a un búfer en RAM con caché antes de interpretarlo
   (`memcpy` NEON o `sceDmacMemcpy`): las lecturas en ráfaga de la CDRAM salen mucho más baratas que
   palabra a palabra.
2. **Hashes más baratos:** muestrear menos (o por generación de escritura) en `vg_tex_hash` y en el
   hash de `vg_vcache_draw`.
3. **Ventana parcial en RAM:** solo si se mide que el juego usa mucho menos de 64 MB contiguos
   (registrar el pico de `MmAllocateContiguousMemory`).

## C2. Ejecutor NV2A: lectura del pushbuffer y despacho de métodos — ⬜

> log_038: ≈ 30 ms/frame fuera de los draws (75 ms del ejecutor − 44,5 de draws). Va detrás de C3.

**Evidencia.** El ejecutor tarda unos 150 ms por fotograma con unos 190 draws. El lado vitaGL
(`compose`/`present`) es de menos de 1 ms. Falta repartir esos 150 ms; los medidores `draws` y
`texturas` de la línea `[HW vita]` lo hacen.

**Pasos.**
1. Con el próximo log: si `draws` ≪ `executing / fotogramas`, el tiempo está en `nv2a_pb_exec.c`
   (upstream: lectura de palabras, `switch` de métodos, `[GPU] 0x0100 ×100 668`).
2. Perfilar con muestreo (Razor o `psvita-toolkit perf-telemetry`) el hilo `0x81026825`.
3. Candidatos: leer el pushbuffer por bloques (no palabra a palabra por `vvm_ptr`), saltar NOPs
   (`0x0100`) en lote, no recalcular estado que no cambió entre draws.

**Cómo medir.** `[KICK] executing` por fotograma.

## C3. Coste por draw en vitaGL — ⬜ (scissor hecho, sin efecto) **prioridad del ejecutor** (log_038: 44,5 ms/frame, 0,29 ms por draw)

> log_038: 152 draws por fotograma y 44,5 ms dentro de `ng_draw_program_batch`, de los que solo
> 3 ms son texturas. `vg_begin_draw` + `vg_state` vuelven a fijar *todo* el estado en cada draw
> (viewport, scissor, máscara de color, mezcla, profundidad, cull, `glUseProgram`, ~10 uniforms,
> 4 texturas y sus samplers, 16 atributos). Primer paso: **caché sombra del estado GL** (llamar solo
> si cambió) y no reenviar uniforms iguales; después medir otra vez.
>
> **Hallazgo (2026-10-30, leyendo vitaGL).** La mayoría de esas llamadas son baratas en vitaGL
> (solo guardan valores), salvo el **scissor**. `glScissor` y `glEnable(GL_SCISSOR_TEST)` lo marcan
> sucio aunque no cambie, y el siguiente draw ejecuta `update_scissor_test` (`tests.c`): 1–2 quads a
> pantalla completa sobre el stencil, más cambios de programa y de uniforms. Como `vg_state` lo fijaba
> en cada draw, eran **2 dibujos extra por cada draw del juego**.
> **Cambio (🔬 en prueba):** `vg_scissor_gl` solo llama a GL si el scissor cambia; todas las
> llamadas del backend pasan por él, y la caché se invalida tras un `glClear` que borre el stencil.
> Medir `draws X` en `[HW vita]` contra log_038 (44,5 ms) y buscar recortes mal hechos (HUD, menús,
> glow). Si sigue alto, el siguiente candidato es el `sceGxmShaderPatcherCreateVertexProgram` que
> vitaGL ejecuta en cada draw (`custom_shaders.c:882`).

**Evidencia.** `nv2a_hw_vita.inc:1374-1383`: cada draw hace 16 llamadas
`glEnable/DisableVertexAttribArray`/`glVertexAttrib4f`, sube las constantes del vertex shader si
cambiaron y pasa los índices como puntero de cliente (vitaGL los copia).

**Pasos.** Solo si C2 muestra que `draws` pesa: recordar qué atributos estaban activos y tocar
solo los que cambian; índices en un VBO del pool; agrupar draws con el mismo estado.

**Cómo medir.** `draws X` en la línea `[HW vita]`.

## C4. Código x86 recompilado (hilo del juego) — 🔬

> **Medida estática (2026-10-30, objdump del ELF):** 15,2 M instrucciones en las `sub_*` y ~1 M accesos
> a memoria guest, cada uno con ~7 instrucciones de traducción (relee `g_xbox_mem_offset`,
> `vvm_ram_lo` y `vvm_ram_size` tras cada escritura volatile): unos 40% del código. GCC ya elimina los
> flags `_fa/_fb` sin uso. TLS: 112 000 llamadas a `__emutls_get_address` (una por registro y tramo
> entre llamadas, no por acceso).
>
> **Paso 1 (🔬):** `XBOX_PTR` → `vvm_gen_ptr` (`compat/vvm.h`): `va < vvm_gen_lim ? va + base : vvm_ptr(...)`,
> con base y límite leídos por un asm sin memoria y fijados en `vvm_fastpath_enable`. 15,23 → 13,22 M
> instrucciones; los caminos de tabla en línea bajan de 510 000 a 220 000. Medir `[THR] game_main`
> contra log_039 (73%).
>
> **Paso 2 (siguiente):** GCC aún recarga base y límite (`movw/movt/ldr`) en muchos accesos. Probar a
> fijarlos en registros reservados (`-ffixed-r9`/`-ffixed-r10` en el código generado) o pasar el
> límite a constante (64 MB) para quitar un `ldr`. Después: TLS de los registros x86 con contexto por
> hilo.

> log_038: `game_main` al 72% ≈ 110 ms/frame. Con el ejecutor más rápido es el siguiente tope (unos 9 fps); lo resuelve C9.

**Evidencia.** El hilo del juego está ocupado el 63% de un núcleo a 5 fps (unos 126 ms por
fotograma), así que es el siguiente tope (unos 8 fps).
- Registros x86 como `RECOMP_TLS`: el binario tiene 44 488 llamadas a `__emutls_get_address`
  (con el atajo de `threads_vita.c`, pero siguen siendo llamadas a función).
- Cada acceso a memoria del Xbox pasa por `vvm_ptr` con `volatile` (`MEM32`…): el compilador
  no puede reutilizar direcciones.

**Pasos.**
1. Medir: perfil por muestreo del hilo del juego (¿cuánto en `__wrap___emutls_get_address`?).
2. Registros en variables globales normales cuando solo un hilo ejecuta código del juego a la vez
   (comprobar que los demás hilos del guest no se ejecutan en paralelo), o pasar el contexto por
   puntero (cambio en el recompilador, upstream).
3. `MEM*` sin `volatile` en el código generado, salvo los rangos MMIO.

**Cómo medir.** `[THR] cpu%` del hilo del juego y `[KICK] waited`.

## C5. Emulación de audio (APU/DSP) — ⬜

> log_038 (hilos ya con nombre): `apu_frame` 25% y `kernel_timer` 22%.

**Evidencia.** Dos hilos (`0x8103bb69` 26% y `0x8101e825` 29% en log_033) que probablemente son la
APU: unos 55% de un núcleo. No está en el camino crítico, pero compite por los 3 núcleos.

**Pasos.** Confirmar qué hilos son (nombrarlos en `threads_vita.c` o `addr2line` con el ELF del
mismo build). Si son la APU: bajar su frecuencia de trabajo o desactivar efectos DSP que no se oigan.

## C6. Memoria al límite — ⬜

**Evidencia.** `[MEM]`: 5 MB de RAM y 0 de CDRAM libres en el menú. C1 cambia el reparto.

**Pasos.** Revisar con el log de C1. Vigilar que vitaGL no fuerce recolecciones (pausas de
segundos en `present`).

## C7. Bloqueo sin KickOffs (log_035) — ⬜

**Evidencia.** log_035: `[KICK] 0 KickOffs` durante 30 s, juego parado. rePOP v0.4.1 corrigió un
cuelgue de pantalla de carga (slots de hilos) y la profundidad de KickOff (16 → 1). Ya está
integrado (submódulo `e86df8c`).

**Pasos.** Ver si se repite con el VPK de 2026-10-30. Si se repite: volcado de hilos en el
momento (como el de Windows de upstream, tras 20 s sin KickOff).

---

# Cambios de fondo (necesarios para una experiencia óptima)

C1–C7 recortan el sobrecoste actual; con ellos se espera llegar, como mucho, a 10–15 fps en el menú
(estimación). Para acercarse a 30 fps hay que quitar trabajo que el Xbox hacía por hardware o en
código nativo. Orden recomendado: **C8 → C9 → C10**. C8 se decide con los números de C2: si la
lectura del pushbuffer es la mayor parte del tiempo del ejecutor, C8 es lo que más rinde.

Esfuerzo: S (días) · M (1–3 semanas) · L (1–2 meses) · XL (más de 2 meses).

## C8. Direct3D 8 en alto nivel (HLE): del juego a vitaGL sin pushbuffer — ⬜ · L–XL

**Por qué.** Hoy el juego ejecuta su librería D3D8 recompilada (secciones `D3D`/`XGRPH` del XBE),
que escribe un pushbuffer NV2A, y nuestro ejecutor lo vuelve a leer y lo traduce a vitaGL. Es trabajo
doble, y además la segunda mitad lee de CDRAM sin caché (C1). Con HLE, las llamadas
`D3DDevice_*` del juego van directo a vitaGL: desaparecen a la vez la generación del pushbuffer
(CPU del juego, C4) y su lectura (ejecutor, C2). Es la técnica de Cxbx-Reloaded.

**Lo que ya existe.** `target_hooks.c` sustituye funciones del juego por dirección (`sub_XXXXXXXX`)
y ya conoce el dispositivo D3D (`D3D_DEVICE_GLOBAL 0x0037B648`). El mecanismo de enganche está
hecho; falta el catálogo de funciones y su implementación.

**Pasos.**
1. **Identificar las funciones D3D8 del XBE** (XDK 5558): usar las firmas de XbSymbolDatabase
   (Cxbx-Reloaded) sobre `default.xbe` → tabla dirección ↔ `D3DDevice_SetTexture`,
   `D3DDevice_DrawIndexedVertices`, `D3DDevice_SetRenderState_*`, `D3DDevice_SetVertexShader`,
   `D3DDevice_Swap`, `D3DResource_Register`, `Lock`/`Unlock`…
2. **Medir cuáles se llaman en el menú** (contador por hook): implementar primero el ~20% que
   hace el ~80% de los draws.
3. **Estado y recursos:** reutilizar lo que ya hay en `nv2a_hw_vita.inc` (shaders de combiners,
   vertex programs en la GPU, caché de texturas y de VBO). Cambia la entrada (argumentos D3D en vez
   de métodos NV2A), no la salida.
4. **Convivencia:** lo que no esté enganchado sigue por el pushbuffer, para poder avanzar función
   a función. El riesgo está en el orden entre ambos caminos (hay que vaciar uno antes de usar el otro).
5. **Límite:** los efectos que el juego programa escribiendo el pushbuffer a mano (si los hay)
   siguen por el ejecutor.

**Cómo medir.** `[KICK] executing` debería tender a 0 en el menú, y `[THR]` del hilo del juego
debería bajar (no genera pushbuffer).

## C9. Recompilación más ligera (upstream: xboxrecomp) — ⬜ · L

**Por qué.** Cada instrucción x86 se traduce a varias operaciones (flags `_fa/_fb/_fas/_fbs`,
`vvm_ptr` con `volatile`, registros como `RECOMP_TLS`, 44 488 llamadas a `__emutls_get_address`).
El hilo del juego tarda unos 126 ms por fotograma, cuando en el Pentium III ese trabajo cabe en menos
de 33 ms.

**Pasos (de menor a mayor esfuerzo).**
1. **Flags muertos:** no calcular `_fa/_fb/_fas/_fbs` cuando la siguiente instrucción que lee
   flags no los usa (análisis de vida dentro del bloque). Suele quitar la mayor parte del código
   generado.
2. **Registros sin TLS:** cargar `g_eax…g_edi` en variables locales al entrar a cada función y
   guardarlos solo en llamadas y salidas (el compilador los mete en registros ARM). Alternativa:
   un puntero de contexto por hilo pasado como argumento.
3. **Memoria sin `volatile`** en el código generado, salvo los rangos MMIO (que ya separa
   `mmio_vita.h`).
4. **x87:** si el juego no depende de la precisión de 80 bits, usar `float` donde el original
   carga y guarda `float` (la Vita es mucho más rápida en precisión simple, con NEON).

**Dónde.** Es un cambio en el generador (`tools/xboxrecomp`, submódulo del repo padre) y obliga a
regenerar `source/xbox-retail/gen`. Conviene proponerlo upstream (rePOP/xboxrecomp) para no
mantener un fork del código generado.

**Cómo medir.** `[THR] cpu%` del hilo del juego y `[KICK] waited`. Probar cada paso por separado
(son cambios de semántica: comparar contra una ejecución de referencia en hosttest ARM32).

## C10. Audio en alto nivel (DirectSound HLE) — ⬜ · M

**Por qué.** La APU/DSP del Xbox se emula en software: unos 55% de un núcleo (C5). En el Xbox era
hardware dedicado.

**Pasos.**
1. Confirmar con C5 qué hilos son la APU y cuánto cuestan.
2. Enganchar las funciones `IDirectSoundBuffer_*` / `DirectSoundCreateStream` del juego (sección
   `DSOUND`; mismas firmas de XbSymbolDatabase) y reproducir los búferes PCM/ADPCM con
   `sceAudioOut` mezclando en ARM con NEON. Los efectos DSP (reverb, `dsstdfx.bin`) se simplifican
   o se omiten.
3. La música y las voces de `soundx*.big` suelen ser streams: priorizarlos.

**Cómo medir.** `[THR] cpu%` de los hilos de la APU hacia 0; el sonido igual de bien.

## Otras versiones del juego (consulta, no hay cambio de plan)

| Versión | CPU / GPU | Encaje con la Vita |
|---|---|---|
| **Xbox (actual)** | x86 little-endian / NV2A con D3D8 | Igual endianness que ARM; D3D8 encaja bien con GL (C8). Ya existe rePOP |
| **PC (2003)** | x86 / D3D8 a través de DLL | La mejor frontera HLE (llamadas a `d3d8.dll` y `dsound.dll`), sin GPU ni APU que emular. Pero no hay recompilación hecha, y los assets son más grandes |
| **GameCube** | PowerPC big-endian / Flipper (GX, TEV) | Cada acceso a memoria necesita un byteswap; GX en alto nivel es factible, pero no hay base hecha |
| **PS2** | MIPS R5900 + VU0/VU1 / GS | Lo peor: microcódigo de VU, registros de 128 bits y blending del GS difícil de traducir a la GPU de la Vita |

---

## Registro de pruebas

| Fecha | VPK / commit | env.txt | Log | fps menú | executing / 10 s | draws (tex) ms/frame | Nota |
|---|---|---|---|---|---|---|---|
| 2026-10-26 | antes de rePOP 0.4.1 | CPU_VSH=0, RENDER_PERCENT=50 | log_033 | 4,9 | 7,3 s | — | Punto de partida |
| 2026-10-30 | C1 + medidores + español | CPU_VSH=0, RENDER_PERCENT=50 | log_036 | — | — | — | Crash al arrancar (6,6 s): `resot_cheats.h` de rePOP 0.4.1 escribía con H + va crudo en EXT_VMA. Corregido (vitaize); C1 y español arrancaron bien hasta ahí |
| 2026-10-30 | + fix resot_cheats.h | CPU_VSH=0, RENDER_PERCENT=50 | log_037 | 0 | — | — | Pantalla negra: sin RAM para los búferes de vértices → rasterizador por software. C1 ❌ |
| 2026-10-30 | ventana en CDRAM otra vez, heap 40, arena 24, medidores, rePOP 0.4.1 | CPU_VSH=0, RENDER_PERCENT=100 | log_038 | **6,5** | 4,9 s | 44,5 (3,0) | Mejora de 4,9 a 6,5 fps. Ejecutor ≈ 75 ms/frame: 44,5 en draws (texturas 3) y ≈ 30 en el pushbuffer. Hilo del juego 72% ≈ 110 ms/frame: ahora es el que marca el ritmo. Español OK (`poplogo2`, `soundx2`) |
| 2026-10-30 | C3: scissor solo si cambia | CPU_VSH=0, RENDER_PERCENT=100 | log_039 | 6,6 | 4,9 s | 44,5 (3,0) | Sin efecto en CPU (el scissor costaba en la GPU). Los 44,5 ms siguen sin localizar: hace falta subdividir el medidor |
| 2026-10-30 | C4: XBOX_PTR barato (vvm_gen_ptr) | CPU_VSH=0, RENDER_PERCENT=100 | log_040 | 5,8 | 4,4 s | 44,7 (3,0) | Sin mejora: `game_main` 76% (antes 73%). 13,22 M instrucciones no se notan; el coste del hilo del juego no está en `XBOX_PTR` (medir por muestreo, C4 paso 1 del plan) |
| 2026-11-04 | rePOP v0.5.0 (renderer directo) | CPU_VSH=0, RENDER_PERCENT=100 | log_041 | — | — | — | "Problema con el disco" a los 5,6 s: abrió `video/pop_in0.int` (inglés). v0.5.0 elige el idioma con `resot_game_language()` (hook `sub_00369310`) y se saltaba `lang_vita.c`. Corregido: `--wrap=resot_game_language`. **Lección:** tras actualizar rePOP, buscar `[LANGUAGE] Xbox=5 Jade=2` en el log |
| 2026-11-04 | + idioma (`--wrap=resot_game_language`) | CPU_VSH=0, RENDER_PERCENT=100 | log_042 | 0 (negro) | — | — | Idioma OK (`Xbox=5 Jade=2`). El juego se para tras el primer flip, antes de `ubisoft.int` (game_main 0 % desde 24 s, 0 KickOffs). Reproducido en hosttest; con `RESOT_DIRECT_DRAW=0` arranca. Renderer directo apagado por defecto en `main_vita.c` hasta arreglarlo |
| 2026-11-04 | + `RESOT_DIRECT_DRAW=0` por defecto | CPU_VSH=0, RENDER_PERCENT=100 | log_043 | 5,6–6,3 | 5,0 s | 43,4–50,7 (3,0) | Base recuperada con rePOP v0.5.0: vídeos en español, menú como log_040. `game_main` 74–76% |

## Otros cambios del VPK de 2026-10-30

- rePOP actualizado a v0.4.1 (`e86df8c`).
- Idioma español: `RECOMP_XC_LANGUAGE=5` por defecto (`compat/lang_vita.c`); el juego abre la
  variante 2 (`soundx2`, `CINE_90x2`, `POPLOGO2`…). Datos subidos con
  `extract_game_data.py … --lang=2`.
