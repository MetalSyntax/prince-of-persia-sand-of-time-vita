# Plan de port: Prince of Persia: The Sands of Time → PS Vita

> Plantilla genérica. Reemplazar por el plan específico del juego tras la Fase 1.
> Formato: `- [ ]` pendiente · `- [x] (AAAA-MM-DD) evidencia` hecho · ⛔ GATE = decisión que puede parar el port.

## Fase 0 — Setup y smoke en consola
- [ ] `docs/xbe.md` e `docs/inventory.md` generados y revisados.
- [ ] `data_manifest.txt` con los archivos mínimos del juego.
- [ ] Smoke VPK compila, instala y da pantalla verde en Vita real con los datos copiados por USB.

## Fase 1 — Viabilidad (¿hay código?)
- [ ] Buscar fuente / decomp / reimplementación existente. Anotar repo, licencia, % y qué corre.
- [ ] ⛔ GATE 1: sin una de las 3 vías (fuente, decomp compilable, reimplementación) el port no es viable.

## Fase 2 — Análisis del XBE y de los datos
- [ ] Ghidra + ghidra-xbe (XbSymbolDatabase) → librerías XDK identificadas; mapa del motor.
- [ ] Formatos de datos: contenedores, texturas (swizzle NV2A), audio, vídeo, scripts.

## Fase 3 — Build nativo en PC (referencia)
- [ ] `source/` compila para x86-64/ARM64 con clang (ABI MSVC-like si viene de MSVC).
- [ ] `port/desktop`: SDL2 + OpenGL, entra al menú y carga un nivel con los datos Xbox.

## Fase 4 — Vita
- [ ] Capa `port/vita`: ventana/GXM vía vitaGL, input, audio (SceAudio), FS (`ux0:data/pop-sot/`).
- [ ] Renderer Vita modelado sobre el OpenGL de referencia; register combiners → shaders.
- [ ] Memoria: presupuesto < ~256 MB; streaming de texturas; caché de shaders.

## Fase 5 — Rendimiento, QA y empaquetado
- [ ] Objetivo 30 fps; resolución 75% configurable; panel `Select+Start`.
- [ ] Vídeos MP4, saves en `ux0:data/pop-sot/save/`, guía de instalación USB.
