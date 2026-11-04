# Mapa del port y plan hasta "jugable" (2026-11-04)

Resumen de una página: dónde está el port y qué falta para que el juego sea jugable en la Vita.
El detalle de cada cuello de botella está en [`PERF_PLAN.md`](PERF_PLAN.md) (C1–C10) y el historial en
[`PORTING_PLAN.md`](PORTING_PLAN.md). Este documento no los sustituye: los ordena.

**"Jugable"** = menú + primer acto (palacio de Azad, Daga, rebobinado, combate) a **≥ 20 fps estables**
(objetivo 30, como en Xbox), sin crashes, con sonido, controles y partidas guardadas.

## 1. Mapa: qué funciona y qué cuesta

Así viaja un fotograma, del código del juego a la pantalla. Medido en consola, menú principal,
`log_040` (2026-10-30): **5,8 fps**.

```text
 ┌─ hilo del juego (game_main) ───────────────────────────────┐
 │ x86 recompilado a C (rePOP)  ── 76 % de un núcleo ≈ 131 ms │ ← tope nº 1
 │   └ librería D3D8 recompilada escribe un pushbuffer NV2A   │
 └──────────────────────────────┬─────────────────────────────┘
                                │ KickOff (el juego espera ≈ 27 % del tiempo)
 ┌─ hilo ejecutor (nv2a_ack) ───▼─────────────────────────────┐
 │ lee el pushbuffer de CDRAM sin caché ≈ 30 ms               │ ← tope nº 2
 │ traduce cada draw a vitaGL (nv2a_hw_vita.inc) ≈ 45 ms      │   (≈ 75 ms en total)
 └──────────────────────────────┬─────────────────────────────┘
                                ▼
          GPU de la Vita: compose + present < 1 ms  ← NO es el cuello
 Audio: APU/DSP del Xbox emulada por software ≈ 55 % de otro núcleo
```

| Pieza | Estado | Evidencia |
|---|---|---|
| Arranque, kernel, hilos, ficheros | ✅ | arranca en consola hasta el menú y la partida |
| Vídeos Bink (FFmpeg) | ✅ fluidos | log_010: 6–10 ms por cuadro |
| Imagen 3D del menú (vitaGL) | ✅ | log_038–040 |
| Idioma español | ✅ | log_038 (`poplogo2.int`, `soundx2.big`) |
| Controles (`input_vita.c`) | ✅ código; ⬜ sin validar jugando | — |
| Partidas guardadas (`ux0:data/pop-sot/saves/`) | ✅ código; ⬜ sin validar | — |
| Audio | ✅ suena; ❌ caro (C5/C10) | `[THR]` |
| **Rendimiento menú** | ❌ 5,8 fps | log_040 |
| **Rendimiento en partida** | ❌ segundos por fotograma | log_012 (antes de las optimizaciones; volver a medir) |
| rePOP (base de código) | ✅ **v0.5.0** `7c2a995` (2026-11-04) | `upstream/rePOP` |

## 2. Por qué Tony Hawk o NFS sí corren y este todavía no

Tienes razón en que **el juego cabe en la Vita**: la Xbox (Pentium III a 733 MHz + NV2A) no está tan lejos
de la Vita (4 × Cortex-A9 a 444 MHz + SGX543MP4). La diferencia está en **cómo** llega el código:

- Los ports jugables (THUG, Halo CE en Vita) parten de **código reescrito o decompilado a C normal**:
  variables en registros ARM y llamadas directas a GL. NFS Most Wanted de Vita es un juego nativo de la Vita.
- Este port ejecuta el **x86 traducido instrucción por instrucción**: cada registro x86 es una variable
  en memoria, cada acceso pasa por una traducción de dirección, se calculan flags que nadie lee, y la GPU
  se **emula** (pushbuffer NV2A) en vez de llamarse directamente. Son 3–4 capas de trabajo que el original
  no tenía. Por eso el hilo del juego tarda ≈ 131 ms en lo que al Pentium III le cuesta < 33 ms.

**El plan quita esas capas una a una.** No es "imposible": hay que cambiar emulación por llamadas
nativas en los tres sitios calientes (GPU, CPU, audio). rePOP ya empezó por la GPU.

## 3. Novedad: rePOP v0.5.0 (actualizado hoy)

De `e86df8c` (v0.4.1) a `7c2a995` (v0.5.0), 15 commits. Lo que importa a la Vita:

- **Renderer directo (v0.4.4, activo por defecto):** los `DrawIndexedVertices` del juego ya no copian los
  índices al pushbuffer; dejan un token de 2 palabras y el ejecutor usa una copia residente. Es la
  primera parte de C8 hecha upstream, y ataca justo el tope nº 2 (lectura del pushbuffer).
  Se apaga con `RESOT_DIRECT_DRAW=0` en `env.txt` si da problemas. Doc: `upstream/rePOP/docs/DIRECT-RENDERER.md`.
- **Juego y renderer en paralelo**, KickOff asíncrono en retail (`RESOT_KICK_AHEAD`).
- Fixes de retail (cuelgue en pantallas de carga por slots de hilo, GPU faults) y soporte de discos europeos.
- Cambio en el port: un ancla de `vitaize.py` (`nv2a_pb_exec.c`, el `#include "nv2a_hw_d3d11.inc"` ahora
  va tras `s_draw_path`).

## 4. Plan hasta jugable

Una fase a la vez; cada una se cierra con un log de consola (menú quieto 60 s, y desde la fase P2 también
los primeros 2 minutos de partida). ⛔ = si falla, se para y se decide.

### P0 — Base al día y medida en partida (S, ahora)
- [x] (2026-11-04) rePOP v0.5.0 + ancla de `vitaize.py` corregida; stubs de los símbolos nuevos de
      upstream (vistas F8, sondas de puertas, turbo Android, `sched_setaffinity`) en `fb_vita.c`/`posix_vita.c`.
      Build OK; eboot subido a `ux0:app/PSOT00001` (13:21).
- [x] (2026-11-04) Prueba en consola: log_041 (idioma inglés → error de disco, corregido), log_042 (renderer
      directo bloquea tras el primer flip, reproducido en hosttest), **log_043: base recuperada, menú 5,6–6,3 fps**
      con `RESOT_DIRECT_DRAW=0` por defecto.
- [ ] Renderer directo: depurar el bloqueo en hosttest (se reproduce) y reactivarlo (va a P1).
- [ ] Probar en consola el eboot con v0.5.0. Buscar `[DIRECT] mode 1` y las líneas `[DIRECT]`/`[DRAWS]`;
      comparar `[KICK] executing` con log_040 (4,4 s / 10 s).
- [ ] **Medir la partida**, no solo el menú: cargar el primer nivel y anotar fps/`[THR]`/`[KICK]`.
      Todo el plan de rendimiento se ha medido en el menú; el objetivo es la partida.
- [ ] Commit del trabajo pendiente del 30-10 (C3, C4, idioma) junto con el submódulo.

### P1 — Ejecutor GPU fuera del camino (M) → objetivo menú ≥ 10 fps
- [ ] Renderer directo de upstream en marcha en la Vita (P0). Si el ejecutor sigue alto, aplicar a
      `nv2a_hw_vita.inc` la caché residente que upstream hace en `nv2a_hw_native.inc` (índices ya
      convertidos por registro: sin hash ni subida por draw).
- [ ] C3: localizar los 45 ms de draws (subdividir el medidor). Sospechoso nº 1:
      `sceGxmShaderPatcherCreateVertexProgram` en cada draw → caché de programas por layout.
- [ ] Leer el pushbuffer por bloques en vez de palabra a palabra desde CDRAM (C2).

### P2 — CPU del juego (L) → objetivo hilo del juego < 50 ms/frame
Es el cambio más grande y el que más da. C4 paso 1 (dirección barata) **no** mejoró (log_040), así que:
- [ ] **Medir antes de tocar:** perfil por muestreo de `game_main` (Razor o `psvita-toolkit perf-telemetry`).
      ¿Cuánto es TLS de registros, flags, accesos a memoria, x87?
- [ ] C9.1 flags muertos y C9.2 registros x86 en locales (sin `__emutls_get_address`). Va en el generador
      (`tools/xboxrecomp`): proponerlo upstream para no mantener un fork del código generado.
- [ ] C9.3 memoria sin `volatile` salvo MMIO.
- [ ] Validar cada paso contra hosttest ARM32 (`./build.sh --hosttest`) antes de la consola.
- [ ] ⛔ **GATE P2:** si con C9 el hilo del juego no baja de ~70 ms/frame, el límite de la recompilación
      está claro y hay que hablar de HLE más alto (funciones de Jade a C nativo, Ruta A parcial).

### P3 — Audio nativo (M) → libera ≈ medio núcleo
- [ ] C10: enganchar DirectSound (`IDirectSoundBuffer_*`, streams) y mezclar en ARM con `sceAudioOut`.
      El núcleo liberado se usa para el ejecutor o para el juego.

### P4 — Jugabilidad (M) → recorrer el primer acto
- [ ] Controles validados en partida (salto, ataque, rebobinado con la Daga, cámara con el stick derecho).
- [ ] Guardar y cargar partida.
- [ ] Recorrido: menú → palacio de Azad → Daga → rebobinado → primer combate, con los crashes resueltos
      uno a uno (`so-crash-triage` con `.psp2dmp`).
- [ ] Memoria: vitaGL va justo (VRAM libre 6–148 KB en log_040); vigilar en niveles grandes.
- [ ] ⛔ **GATE 3R.2:** menú + primer acto ≥ 20 fps estables y sin crash.

### P5 — Pulido (S–M)
- Resolución configurable (`RECOMP_RENDER_PERCENT`), overclock a 444 MHz confirmado, LiveArea, README de release.

## 5. Expectativa honesta

| Tras | Menú (estimado) | Base |
|---|---|---|
| hoy | 5,8 fps | log_040 |
| P1 | 8–10 fps | el juego pasa a ser el único tope (≈ 131 ms/frame) |
| P1 + P2 | 15–25 fps | si C9 recorta el hilo del juego a la mitad o menos |
| P1 + P2 + P3 | 20–30 fps | un núcleo más libre |

Son estimaciones, no medidas: cada fase se confirma con su log antes de pasar a la siguiente.

## 6. Decompilación híbrida sobre la recompilación (pedido 2026-11-04)

**Idea:** no decompilar el juego entero (son 14 752 funciones; GATE 1 lo descarta como proyecto de
años), sino **reescribir en C nativo solo las funciones donde el hilo del juego gasta el tiempo**, y
dejar el resto recompilado. Es lo mismo que hace rePOP con sus hooks, pero para ganar velocidad.

**Por qué no un "loader" (como los ports de Android con soloader):** un loader carga código que la CPU
ya entiende (`.so` ARM). El `default.xbe` es x86: la Vita no puede ejecutarlo sin traducirlo, y esa
traducción es justo la recompilación que ya tenemos. El *loader* de este port ya existe: rePOP.
(Nota: `.xex` es de Xbox 360; el disco de Xbox clásica trae `default.xbe`, que es lo que hay en
`game_data_ntsc/` y lo que hay que abrir en Ghidra con la extensión `ghidra-xbe`.)

**Mecanismo, ya hecho en rePOP:** la función generada se renombra `sub_XXXXXXXX_gen` y
`source/xbox-retail/target_hooks.c` define `sub_XXXXXXXX` con C escrito a mano (ej. `sub_0001E970`).
Para la Vita, nuestras sustituciones irían en `port/vita/` (el submódulo no se edita).

**Bucle por función:**
1. **Elegir por medida:** perfil por función del hilo del juego (muestreo de PC en el hosttest ARM32
   o en consola) → las ~50 funciones que suman la mayor parte del tiempo. Candidatos típicos en Jade:
   matrices/vectores, skinning, colisión, el intérprete de IA (`ai/`), el renderer Jade (`render/`).
2. **Decompilar** esa función en Ghidra (con nombres y tipos de las notas de rePOP) y comparar con su
   C generado en `source/xbox-retail/gen/<módulo>/`.
3. **Reescribir** en C normal: argumentos leídos una vez de la pila guest, variables locales en
   registros ARM, `float` y NEON en vez de x87, y punteros guest traducidos una sola vez.
4. **Validar por diferencias:** ejecutar `_gen` y la versión nativa con las mismas entradas (en el
   hosttest) y comparar retorno y memoria escrita. Sin esto no se activa.
5. **Medir** `[THR] game_main` en consola. Un interruptor por `env.txt` permite volver a `_gen`.

- [ ] D0 Abrir `game_data_ntsc/default.xbe` en Ghidra (`ghidra-xbe`) con el plugin MCP activo
      (`http://127.0.0.1:8080`).
- [ ] D1 Perfilador por función del hilo del juego → lista de las funciones calientes.
      Hecho el instrumento (2026-11-04): `HOSTTEST_PROF=1 port/vita/hosttest/run.sh run 900` y luego
      `port/vita/hosttest/run.sh prof` → `build/hosttest-data/logs/prof_report.md`
      (`hosttest/prof_report.py --self-check` OK). Falta la primera corrida.
- [ ] D2 Primera función sustituida y validada por diferencias (elegir una hoja matemática).
- [ ] D3 Las 10 más calientes → medir. ⛔ Si no bajan `game_main` de forma visible, parar y revisar.
- No usar el código filtrado de Jade (decisión del usuario requerida, ver `CLAUDE.md`).
