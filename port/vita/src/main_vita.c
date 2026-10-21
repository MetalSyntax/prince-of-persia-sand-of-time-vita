/**
 * reSOT Vita - punto de entrada del host, contraparte de
 * upstream/rePOP/port/android/src/main_android.c.
 *
 * El disco NTSC extraído va en ux0:data/pop-sot/ (default.xbe con el SHA-256
 * de target.h, prince.bf, shaders.bin, VIDEO/, SOUND/, MEDIA/). Logs en
 * ux0:data/pop-sot/logs/ (la ejecución anterior se conserva como *-prev.txt).
 *
 * El hilo principal lleva la pantalla y el mando; el punto de entrada elevado
 * corre en un hilo propio con pila grande, como en Android.
 */
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>

#include "platform/win32_compat.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "target.h"
#include "host_files.h"
#include "sha256.h"
#include "options.h"
#include "vvm.h"
#include "manifest.h"

/* newlib: el heap del runtime (vídeo, sonido, archivos, vértices del backend
 * GPU). Lo grande (bloque guest, ventana contigua, arena EXT_VMA) lo pide
 * compat/vvm.c aparte con sceKernelAllocMemBlock, ya con el log abierto: con
 * 256 MB aquí la Vita no tenía memoria y newlib fallaba antes de main, sin log
 * (primer .psp2dmp en consola, 2026-10-20: LR en _init_vita_heap). */
int _newlib_heap_size_user = 48 * 1024 * 1024;
/* pthread_create sin atributos: 32 KB en VitaSDK (1 MB en Android). Los hilos
 * de temporizador del runtime ejecutan código del juego (DPC/APC). */
unsigned int _pthread_stack_default_user = 1024 * 1024;

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern void xbe_entry_point(void);
extern void xbox_OhciInit(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern void vita_fb_init(void);
extern void vita_fb_message(uint32_t rgb);
extern void vita_fb_pump(void);
extern void vita_input_init(void);

static volatile int s_game_running;

const char *resot_android_data_dir(void) { return DATA_DIR; }

/* logs/log_NNN.log: el siguiente al número más alto que exista (001..999,
 * después vuelve a 001). stdout y stderr van al mismo archivo, en modo append. */
static void open_log(void)
{
    char path[256];
    int last = 0, n;
    FILE *f;
    DIR *d = opendir(DATA_DIR "/logs");
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        if (sscanf(e->d_name, "log_%3d.log", &n) == 1 && n > last && n <= 999) last = n;
    if (d) closedir(d);
    snprintf(path, sizeof path, DATA_DIR "/logs/log_%03d.log", last % 999 + 1);
    if ((f = fopen(path, "w")) != NULL) fclose(f);
    freopen(path, "a", stderr);
    freopen(path, "a", stdout);
}

static void fail(const char *message)
{
    fprintf(stderr, "[BOOT] %s\n", message);
    fflush(stderr);
    vita_fb_message(0x800000);   /* pantalla roja: detalle en logs/ */
    s_game_running = 0;
}

static const char *missing_game_file(void)
{
    int i;
    for (i = 0; kRequired[i]; i++) {
        if (!resot_path_kind(DATA_DIR, kRequired[i], NULL, 0)) {
            /* rutas con carpeta (SOUND/x.big): resot_path_kind solo mira un nivel */
            char path[512];
            SceIoStat st;
            snprintf(path, sizeof path, DATA_DIR "/%s", kRequired[i]);
            if (sceIoGetstat(path, &st) < 0)
                return kRequired[i];
        }
    }
    return NULL;
}

static int load_xbe(void **out_data, size_t *out_size, char *why, size_t why_size)
{
    char path[512], hex[65];
    ResotSha256 sha;
    FILE *f;
    long size;
    void *data;
    if (!resot_path_kind(DATA_DIR, "default.xbe", path, sizeof path))
        snprintf(path, sizeof path, DATA_DIR "/default.xbe");
    if (!(f = fopen(path, "rb"))) { snprintf(why, why_size, "No se puede abrir %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = size > 0 ? malloc((size_t)size) : NULL;
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(data);
        snprintf(why, why_size, "No se puede leer %s", path);
        return 0;
    }
    fclose(f);
    /* Cada dirección del código elevado es de esta revisión exacta (NTSC 2003-10-20). */
    resot_sha256_init(&sha);
    resot_sha256_update(&sha, data, (size_t)size);
    resot_sha256_hex(&sha, hex);
    if (strcasecmp(hex, RESOT_XBE_SHA256) != 0) {
        free(data);
        snprintf(why, why_size, "default.xbe no es el %s NTSC que soporta el port: "
                 "SHA-256 %s, se esperaba %s", RESOT_TARGET_ID, hex, RESOT_XBE_SHA256);
        return 0;
    }
    *out_data = data;
    *out_size = (size_t)size;
    return 1;
}

static void log_free_memory(const char *when)
{
    SceKernelFreeMemorySizeInfo fm = { sizeof fm };
    sceKernelGetFreeMemorySize(&fm);
    struct mallinfo mi = mallinfo();   /* arena = pico del heap (newlib no lo devuelve) */
    fprintf(stderr, "[MEM] %s: libre user %u KB, cdram %u KB, phycont %u KB; heap newlib %u MB, "
            "en uso %u KB, pico %u KB\n",
            when, fm.size_user >> 10, fm.size_cdram >> 10, fm.size_phycont >> 10,
            (unsigned)(_newlib_heap_size_user >> 20), (unsigned)mi.uordblks >> 10, (unsigned)mi.arena >> 10);
}

static void set_default(const char *name, const char *value)
{
    if (!getenv(name)) setenv(name, value, 1);
}

/* En la consola no hay variables de entorno: DATA_DIR/env.txt, una CLAVE=valor
 * por línea (# comenta), se aplica antes de los valores por defecto. */
static void load_env_file(void)
{
    char line[256];
    FILE *f = fopen(DATA_DIR "/env.txt", "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *end = line + strcspn(line, "\r\n");
        *end = 0;
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "[BOOT] env.txt: %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

static void *game_main(void *unused)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    char why[512];
    const char *missing;
    (void)unused;

    if ((missing = missing_game_file()) != NULL) {
        snprintf(why, sizeof why, "Falta %s en " DATA_DIR " (copiar ahí el disco NTSC extraído)", missing);
        fail(why);
        return NULL;
    }
    if (!load_xbe(&xbe_data, &xbe_size, why, sizeof why)) {
        fail(why);
        return NULL;
    }

    printf("=== " RESOT_TARGET_NAME " - recompilación estática (PS Vita) ===\n");
    fprintf(stderr, "[SESSION] target=%s ext-vma=%s xbe=%s\n", RESOT_TARGET_ID,
            getenv("RECOMP_EXT_VMA") ? getenv("RECOMP_EXT_VMA") : "0", RESOT_XBE_SHA256);

    if (!vvm_init() || !xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fail("No se pudo preparar la memoria guest (ver logs)");
        free(xbe_data);
        return NULL;
    }
    fprintf(stderr, "[BOOT] memoria guest lista: offset 0x%08X, %u MB reales\n",
            (unsigned)g_xbox_mem_offset, (unsigned)(vvm_committed_bytes() >> 20));
    log_free_memory("tras el layout");

    /* La APU emulada (registros por compat/mmio_vita.h): sin ella el motor no
     * inicializa el sonido y crashea más tarde (xbox_memory_layout.c, codec). */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] APU emulada %s\n", g_apu_state ? "lista" : "FALLÓ");
    }

    xbox_kernel_init();
    xbox_path_init(DATA_DIR, DATA_DIR "/saves/" RESOT_TARGET_ID);
    xbox_kernel_bridge_init();
#ifdef RESOT_D3D_DEVICE_GLOBAL
    xbox_Nv2aMirrorFence(RESOT_D3D_DEVICE_GLOBAL, RESOT_D3D_FENCE_ISSUED, RESOT_D3D_FENCE_GPU_PTR);
#endif
#ifdef RESOT_DSOUND_CODE_BEGIN
    xbox_SetApuPhysicalCallers(RESOT_DSOUND_CODE_BEGIN, RESOT_DSOUND_CODE_END);
#endif
    xbox_OhciInit();
    g_esp = XBOX_STACK_TOP;
    /* Sin recomp_dispatch_init(): su tabla plana son 30 MB del heap de newlib.
     * Las llamadas indirectas usan la caché de compat/dispatch_cache_vita.c. */
    xbox_WatchdogStart();

    printf("\nIniciando el juego...\n");
    fflush(stdout);
    fflush(stderr);
    xbe_entry_point();
    printf("\nEl juego terminó.\n");
    fflush(stdout);
    fflush(stderr);
    s_game_running = 0;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_attr_t attr;

    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(DATA_DIR "/logs", 0777);
    sceIoMkdir(DATA_DIR "/saves", 0777);
    open_log();
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);   /* ponytail: por línea, para no perder el final al crashear */
    chdir(DATA_DIR);

    /* Máximo reloj: el juego corre en la CPU (ARM Cortex-A9). */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    /* Los valores de trabajo de upstream (main_android.c), con el encoder GPU
     * de la Vita (src/nv2a_hw_vita.inc, vitaGL) en lugar del de Vulkan. */
    load_env_file();
    set_default("RECOMP_PB_EXEC", "1");
    set_default("RECOMP_FB_WINDOW", "1");
    set_default("RECOMP_VBLANK", "1");
    /* Por software hasta validar vitaGL en la consola: en Vita3K (0.2.1-4210, macOS)
     * nada de vitaGL llega a la pantalla, ni un glClear (2026-10-21). RECOMP_HW=1 en env.txt. */
    set_default("RECOMP_HW", "0");
    if (!strcmp(getenv("RECOMP_HW"), "0"))
        unsetenv("RECOMP_HW");   /* upstream solo mira si existe: "0" = rasterizador por software */
    set_default("RECOMP_HW_SCALE", "1");   /* 640x480 nativos: x2 no cabe en la GPU de la Vita */
    /* Rasterizador por software (sin RECOMP_HW): upstream usa núcleos-4 hilos
     * (en Vita, 1). ponytail: 3 = principal + 2 trabajadores, sin medir en consola. */
    set_default("RECOMP_RASTER_THREADS", "3");
    set_default("RECOMP_HEAP_RECLAIM", "1");
    /* APU emulada, como Android: sus registros van por compat/mmio_vita.h. */
    set_default("RECOMP_AC97_READY", "1");
    set_default("RECOMP_APU_DSP_ACK", "gp+0x810");
#if defined(RESOT_DEFAULT_EXT_VMA) && RESOT_DEFAULT_EXT_VMA
    set_default("RECOMP_EXT_VMA", "1");
#endif

    fprintf(stderr, "[BOOT] %s, datos en %s\n", RESOT_TARGET_ID, DATA_DIR);
    log_free_memory("al arrancar");
    vita_fb_init();
    vita_input_init();
    resot_options_init();

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    s_game_running = 1;
    if (pthread_create(&thread, &attr, game_main, NULL) != 0) {
        fail("No se pudo crear el hilo del juego");
    }
    pthread_attr_destroy(&attr);

    for (unsigned tick = 1;; tick++) {
        vita_fb_pump();   /* muestra el último frame y atiende el mando */
        if (tick % 600 == 0) log_free_memory("cada 10 s");
        if (!s_game_running) {
            fflush(stdout);
            fflush(stderr);
        }
        sceKernelDelayThread(16000);
    }
    return 0;
}
