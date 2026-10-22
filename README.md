# Prince of Persia: The Sands of Time — PS Vita Port

Port nativo de **Prince of Persia: The Sands of Time** (Xbox Clásica) para **PlayStation Vita**, basado en recompilación estática a través de [rePOP / reSOT](https://github.com/FulGerNet/rePOP).

> [!NOTE]
> Este port no es un emulador ni un cargador dinámico de Android (`.so loader`). El código x86 del juego original de Xbox se recompila estáticamente a código nativo ARM para el procesador Cortex-A9 de PS Vita, y las llamadas gráficas NV2A / Direct3D 8 se traducen directamente a hardware GXM mediante **vitaGL**.

---

## 🎮 Estado Actual del Proyecto (WIP - Fase Alpha)

- **Progreso actual:** El port arranca con éxito, procesa la inicialización de subsistemas y **llega al menú principal renderizando el escenario 3D completo de fondo**.
- **Rendimiento:** **~20–23 FPS** en el menú dentro del emulador **Vita3K** (en un Mac Mini M4).
- **Gráficos:** El entorno 3D (palacio, palmeras, cielo nocturno/atardecer, iluminación y niebla volumétrica) se renderiza de forma continua y estable en la GPU mediante shaders de hardware, sin caídas a CPU ni parpadeos.
- **Pendiente:** Transición a gameplay 3D interactivo, sincronización completa de audio APU/DSound, y optimizaciones adicionales para alcanzar 30 FPS estables en la consola física.

---

## 📋 Requisitos para la Consola (PS Vita Real)

1. PS Vita con **HENkaku / Enso** (firmware 3.60 o 3.65 recomendado).
2. Plugin y runtime de compilación de shaders: **`libshacccg.suprx`** instalado en `ur0:data/` (se puede extraer fácilmente usando la app homebrew [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D)).
3. Espacio en la tarjeta de memoria (`ux0:`).

---

## 📂 Archivos del Juego (Data Files)

Por razones legales anti-DMCA, este repositorio **no contiene ningún archivo propietario del juego**. Debes suministrar los datos de tu copia legítima de Xbox (se recomienda versión NTSC o PAL):

- Carpeta destino en PS Vita: `ux0:data/pop-sot/`
- **Title ID:** `PSOT00001`
- Archivos requeridos (extraídos del disco / XISO):
  - `prince.bf` (archivo principal de datos del motor Jade)
  - Carpeta `video/` con los archivos Bink (`.int`)
  - Configuración y datos retail según se describe en [`data_manifest.txt`](file:///Volumes/TOSHIBA%20EXT/Xbox/xbox-vita-ports/ports/pop-sot/data_manifest.txt).

---

## 🛠️ Compilación desde el Código Fuente

### Requisitos de entorno
- [VitaSDK](https://vitasdk.org/) instalado y configurado en el `PATH` o `$VITASDK`.
- `cmake` (>= 3.20) y `ninja` o `make`.
- `python3`.

### Comandos de compilación

```bash
# 1. Clonar el repositorio con sus submódulos
git clone --recursive https://github.com/tu-usuario/pop-sot-vita.git
cd pop-sot-vita

# 2. Compilar el port completo (.vpk y eboot.bin)
./build.sh

# 3. Compilar y ejecutar directamente en Vita3K (pruebas locales)
./build.sh --vita3k
```

El script `./build.sh` generará el paquete listo para instalar en:
- `build/vita-port/pop-sot.vpk`
- `build/vita-port/eboot.bin`

---

## 🏗️ Arquitectura Técnica

- **Traductor NV2A a vitaGL:** Pipeline gráfico en [nv2a_hw_vita.inc](file:///Volumes/TOSHIBA%20EXT/Xbox/xbox-vita-ports/ports/pop-sot/port/vita/src/nv2a_hw_vita.inc). Traduce programas de vértices y registros combiners de Nvidia a shaders de fragmentos CG y VBOs de vitaGL.
- **16 Atributos de Vértice en GPU:** Empaquetador `nv2a_vsh_pack_gpu` con soporte completo de 16 streams, eliminando el 100% de caídas al rasterizador por CPU (`a CPU 0/0/0`).
- **Gestión de Memoria en PS Vita:**
  - Heap de Newlib ajustado a **56 MB**.
  - Búfer de trabajo de VBOs en bloque de memoria independiente (`pop-batch-scratch`, 2 MB).
  - Circular Pool de vitaGL ampliado a **12 MB** para evitar saturación de comandos GXM.
  - VBO Cache de geometría estática para reducir la carga de CPU por fotograma.

---

## 👥 Créditos y Agradecimientos

- **FulGerNet** y colaboradores de [rePOP](https://github.com/FulGerNet/rePOP) por el proyecto original de recompilación estática de Prince of Persia: The Sands of Time.
- **Rinnegatamante** por [vitaGL](https://github.com/Rinnegatamante/vitaGL) y sus constantes aportes a la escena homebrew de PS Vita.
- **Equipo de VitaSDK** por la toolchain libre de desarrollo.
- **Equipo de Vita3K** por el emulador de PS Vita.
