# Prince of Persia: The Sands of Time (PS Vita) — Notas de la Versión

## 🏷️ Versión: v0.1.0-alpha — Hito: Menú 3D Estable (Menu Milestone)

**Fecha:** 22 de octubre de 2026  
**Title ID:** `PSOT00001`  
**Rendimiento de Referencia:** **~20–23 FPS** en Vita3K (Mac Mini M4) / Menú 3D en tiempo real

---

### 🌟 Resumen del Lanzamiento

Esta versión preliminar marca el primer gran hito funcional del port de Xbox Clásica a PS Vita: el juego inicializa todos sus sistemas centrales, carga los assets desde la tarjeta de memoria, procesa los vídeos de introducción y **renderiza de manera estable y continua el entorno tridimensional completo del menú principal**.

---

### ✅ Lo que Funciona en Esta Versión

1. **Arranque y Carga de Motor:**
   - Montaje del sistema de archivos y lectura de datos desde `ux0:data/pop-sot/`.
   - Inicialización del subsistema de memoria guest y thunks de kernel Xbox.
   - Decodificación y reproducción de vídeos Bink (`.int`) mediante decodificador adaptado.

2. **Renderizado Gráfico 3D en Hardware:**
   - Escenario 3D del menú principal completamente visible: palacio, palmeras, iluminación atmosférica, antorchas, arena y cielo al atardecer.
   - **0 caídas a CPU en shaders de vértices (`a CPU 0/0/0`):** Soporte de 16 atributos de vértice procesados 100% en la GPU mediante vitaGL.
   - Estabilidad de imagen total: corregido el problema de parpadeos intermitentes y el bug de pantalla negra que borraba la escena.

3. **Optimizaciones de Memoria y Estabilidad:**
   - Eliminados los bloqueos por agotamiento de circular pool de vitaGL (ampliado a 12 MB).
   - Heap de Newlib ajustado a 56 MB para evitar fugas y fallos de asignación en consola real.
   - Búfer de trabajo de VBOs en bloque de memoria independiente de 2 MB.

---

### ⚠️ Problemas Conocidos y Trabajo Pendiente

- **Entrada a Gameplay:** La transición desde el menú principal hacia el inicio de una nueva partida / juego 3D interactivo se encuentra aún en desarrollo y depuración de llamadas del motor Jade.
- **Audio / APU:** La emulación de canales de audio DSound/APU y la sincronización de pistas musicales y voces aún requiere ajustes.
- **Tasa de Cuadros:** En Vita3K (Mac Mini M4) el menú corre actualmente a ~23 FPS; el objetivo para la consola física es alcanzar 30 FPS estables mediante mayor optimización de render passes y texturas.

---

### 📥 Instrucciones de Instalación

#### En Consola Física PS Vita
1. Asegúrate de tener instalado **HENkaku / Enso** y el archivo `ur0:data/libshacccg.suprx` (instalable con ShaRKF00D).
2. Instala el paquete [`pop-sot.vpk`](file:///Volumes/TOSHIBA%20EXT/Xbox/xbox-vita-ports/ports/pop-sot/build/vita-port/pop-sot.vpk) mediante VitaShell.
3. Copia los archivos de juego legítimos de tu copia de Xbox en `ux0:data/pop-sot/`.
4. Inicia el juego desde la LiveArea.

#### En Emulador Vita3K
1. Abre Vita3K e instala `pop-sot.vpk` (o copia el ejecutable `eboot.bin` en `ux0:app/PSOT00001/`).
2. Coloca los archivos del juego en la ruta de datos del emulador:
   `Vita3K/fs/ux0/data/pop-sot/`
3. Ejecuta el título `PSOT00001`.

---

### 🔧 Registro de Cambios Técnicos Destacados

- **Fix en `ng_clear`:** Se corrigió la condición de limpieza de búferes de color en [nv2a_hw_vita.inc](file:///Volumes/TOSHIBA%20EXT/Xbox/xbox-vita-ports/ports/pop-sot/port/vita/src/nv2a_hw_vita.inc). Anteriormente, limpiezas de solo canal alfa (`param 0x80` / `0x83`) provocaban un `glClear(GL_COLOR_BUFFER_BIT)` que borraba la escena 3D a negro.
- **Soporte de 16 Atributos:** Modificado el empaquetador `nv2a_vsh_pack_gpu` en [tools/vitaize.py](file:///Volumes/TOSHIBA%20EXT/Xbox/xbox-vita-ports/ports/pop-sot/port/vita/tools/vitaize.py) y shaders CG en [nv2a_hw_vita.inc](file:///Volumes/TOSHIBA%20EXT/Xbox/xbox-vita-ports/ports/pop-sot/port/vita/src/nv2a_hw_vita.inc) para aceptar hasta 16 atributos de entrada (`a_in0` a `a_in15`).
- **Sincronización FBO:** Añadido `glFinish()` en `ng_compose_body()` para asegurar que la GPU GXM resuelva el render target antes de que la pantalla lo proyecte.
