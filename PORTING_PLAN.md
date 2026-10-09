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

### Fase 3B — Prototipo de recompilación estática (solo si el usuario lo aprueba en el GATE 1)

- [ ] 3B.1 Elegir 1 módulo pequeño. Traducir su código x86 a C automáticamente (registros como variables,
      memoria plana). Compilar para ARM y medir la equivalencia contra xemu.
- [ ] 3B.2 Capa HLE mínima de D3D8 sobre vitaGL para el frontend.
- [ ] ⛔ **GATE 3B:** el frontend se renderiza en la Vita a ≥ 30 fps con el módulo traducido. Si no → abandonar 3B.

## Fase 4 — Build portable en PC (referencia)

Requiere la Fase 3 al 100 % (o 3B validada).

- [ ] 4.1 `port/desktop`: SDL2 + OpenGL; D3D8 → GL siguiendo el patrón de `halo-ce-universal`; DSOUND → SDL audio;
      Bink → MP4 o saltar.
- [ ] 4.2 Hitos con captura: menú → primer nivel (palacio de Azad) → Daga del Tiempo (rebobinado) → combate →
      guardado en un punto de guardado.
- [ ] ⛔ **GATE 4:** el primer acto es jugable en escritorio con datos Xbox, sin crashes.

## Fase 5 — Vita

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
