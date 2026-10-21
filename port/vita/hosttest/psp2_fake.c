/* hosttest: las llamadas sce* del host Vita sobre Linux ARM32 (glibc), para
 * arrancar el mismo binario (vvm, vitaize, C generado) en un contenedor armv7
 * antes de ir a la consola. No es un emulador de Vita: pantalla = PPM en
 * DATA_DIR/logs/frames/, audio = descartado a tiempo real, mando = guion.
 *
 * HOSTTEST_PAD="<ms>:<botón>[:<ms pulsado>],..."  botón: cross circle square
 * triangle start select up down left right l r. Tiempos desde el arranque.
 * HOSTTEST_FRAME_MS: intervalo entre capturas (por defecto 2000).
 * Con DISPLAY definido además abre una ventana en vivo con teclado (window_sdl.c).
 */
/* psp2 antes que glibc: <sys/stat.h> define st_atime como macro. */
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "window_sdl.h"

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t s_t0;
__attribute__((constructor)) static void start_clock(void) { s_t0 = now_ms(); }

/* ── archivos, energía, memoria ─────────────────────────────── */

int sceIoMkdir(const char *dir, SceMode mode) { return mkdir(dir, mode) == 0 || errno == EEXIST ? 0 : -1; }
int sceIoGetstat(const char *file, SceIoStat *st)
{
    struct stat s;
    if (stat(file, &s) != 0) return -1;
    memset(st, 0, sizeof *st);
    st->st_size = s.st_size;
    st->st_mode = S_ISDIR(s.st_mode) ? SCE_S_IFDIR : SCE_S_IFREG;
    return 0;
}
int scePowerSetArmClockFrequency(int f) { (void)f; return 0; }
int scePowerSetBusClockFrequency(int f) { (void)f; return 0; }
int scePowerSetGpuClockFrequency(int f) { (void)f; return 0; }
int scePowerSetGpuXbarClockFrequency(int f) { (void)f; return 0; }

int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info)
{
    long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    info->size_user = (int)((unsigned long)pages * page > 0x7FFFFFFF ? 0x7FFFFFFF : pages * page);
    info->size_cdram = info->size_phycont = 0;
    return 0;
}

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type, SceSize size,
                              SceKernelAllocMemBlockOpt *opt)
{
    static void *blocks[16];
    static int n;
    (void)name; (void)type; (void)opt;
    if (n == 16 || !(blocks[n] = calloc(1, size))) return -1;
    return (SceUID)(intptr_t)&blocks[n++];
}
int sceKernelGetMemBlockBase(SceUID uid, void **base) { *base = *(void **)(intptr_t)uid; return 0; }

/* ── hilos ──────────────────────────────────────────────────── */

typedef struct { pthread_t t; SceKernelThreadEntry entry; SceSize stack; } fake_thread;

static void *thread_main(void *arg)
{
    fake_thread *ft = arg;
    return (void *)(intptr_t)ft->entry(0, NULL);
}
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int prio, SceSize stack,
                             SceUInt attr, int affinity, const SceKernelThreadOptParam *opt)
{
    fake_thread *ft = calloc(1, sizeof *ft);
    (void)name; (void)prio; (void)attr; (void)affinity; (void)opt;
    ft->entry = entry;
    ft->stack = stack;
    return (SceUID)(intptr_t)ft;
}
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp)
{
    fake_thread *ft = (fake_thread *)(intptr_t)thid;
    pthread_attr_t a;
    (void)arglen; (void)argp;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, ft->stack < 65536 ? 65536 : ft->stack);   /* misma pila que en la Vita */
    return pthread_create(&ft->t, &a, thread_main, ft) ? -1 : 0;
}
int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout)
{
    (void)stat; (void)timeout;
    return pthread_join(((fake_thread *)(intptr_t)thid)->t, NULL);
}
int sceKernelDeleteThread(SceUID thid) { free((void *)(intptr_t)thid); return 0; }
int sceKernelDelayThread(SceUInt delay) { usleep(delay); return 0; }

/* ── pantalla: una captura PPM cada HOSTTEST_FRAME_MS ───────── */

int sceDisplaySetFrameBuf(const SceDisplayFrameBuf *fb, SceDisplaySetBufSync sync)
{
    static uint64_t next;
    static unsigned shot;
    static int every = -1;
    char path[256];
    const uint32_t *px = fb->base;
    FILE *f;
    int i;
    (void)sync;
    hosttest_window_frame(fb->base, fb->width, fb->height, fb->pitch);
    if (every < 0) every = getenv("HOSTTEST_FRAME_MS") ? atoi(getenv("HOSTTEST_FRAME_MS")) : 2000;
    if (now_ms() < next) return 0;
    next = now_ms() + (uint64_t)every;
    mkdir(DATA_DIR "/logs/frames", 0777);
    snprintf(path, sizeof path, DATA_DIR "/logs/frames/%04u-%06llus.ppm", shot++,
             (unsigned long long)((now_ms() - s_t0) / 1000));
    if (!(f = fopen(path, "wb"))) return 0;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)fb->width, (unsigned)fb->height);
    for (i = 0; i < fb->pitch * fb->height; i++) {
        uint32_t p = px[i];   /* A8B8G8R8 */
        unsigned char rgb[3] = { (unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16) };
        if (i % fb->pitch < fb->width) fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

/* ── mando por guion ────────────────────────────────────────── */

int sceCtrlSetSamplingMode(SceCtrlPadInputMode mode) { (void)mode; return 0; }

static unsigned button(const char *name)
{
    static const struct { const char *n; unsigned b; } k[] = {
        {"cross", SCE_CTRL_CROSS}, {"circle", SCE_CTRL_CIRCLE}, {"square", SCE_CTRL_SQUARE},
        {"triangle", SCE_CTRL_TRIANGLE}, {"start", SCE_CTRL_START}, {"select", SCE_CTRL_SELECT},
        {"up", SCE_CTRL_UP}, {"down", SCE_CTRL_DOWN}, {"left", SCE_CTRL_LEFT},
        {"right", SCE_CTRL_RIGHT}, {"l", SCE_CTRL_LTRIGGER}, {"r", SCE_CTRL_RTRIGGER}};
    unsigned i;
    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(name, k[i].n)) return k[i].b;
    return 0;
}

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int count)
{
    const char *script = getenv("HOSTTEST_PAD");
    uint64_t t = now_ms() - s_t0;
    (void)port; (void)count;
    memset(pad, 0, sizeof *pad);
    pad->lx = pad->ly = pad->rx = pad->ry = 128;
    while (script && *script) {
        unsigned long at = 0, hold = 200;
        char name[16] = "";
        sscanf(script, "%lu:%15[a-z]:%lu", &at, name, &hold);
        if (t >= at && t < at + hold) pad->buttons |= button(name);
        script = strchr(script, ',');
        if (script) script++;
    }
    hosttest_window_pad(&pad->buttons, &pad->lx, &pad->ly);
    return 1;
}

int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state) { (void)port; (void)state; return 0; }
int sceTouchPeek(SceUInt32 port, SceTouchData *data, SceUInt32 n) { (void)port; (void)n; memset(data, 0, sizeof *data); return 1; }

/* ── audio: se descarta al ritmo real (la APU mide su cola con esto) ── */

static int s_audio_len = 256, s_audio_freq = 48000;
int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode)
{
    (void)type; (void)mode;
    s_audio_len = len;
    s_audio_freq = freq;
    return 0;
}
int sceAudioOutOutput(int port, const void *buf)
{
    (void)port; (void)buf;
    usleep((useconds_t)(1000000LL * s_audio_len / s_audio_freq));
    return 0;
}
int sceAudioOutReleasePort(int port) { (void)port; return 0; }

/* ── crashes: lo que en la Vita da el .psp2dmp ──────────────── */

extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;

/* Offset dentro del ELF (es PIE) para addr2line; 0 si es de otra biblioteca. */
static unsigned long in_elf(unsigned long addr, const char **lib)
{
    Dl_info info;
    *lib = "?";
    if (!dladdr((void *)addr, &info) || !info.dli_fname) return 0;
    *lib = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
    return addr - (unsigned long)info.dli_fbase;
}

static void on_crash(int sig, siginfo_t *si, void *uctx)
{
    ucontext_t *uc = uctx;
    const char *lp, *ll;
    unsigned long op = in_elf(uc->uc_mcontext.arm_pc, &lp), ol = in_elf(uc->uc_mcontext.arm_lr, &ll);
    fprintf(stderr, "[CRASH] pc %s+0x%lX  lr %s+0x%lX\n", lp, op, ll, ol);
    fprintf(stderr, "[CRASH] señal %d pc=0x%08lX lr=0x%08lX addr=%p\n"
            "  guest eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n"
            "  (arm-linux-gnueabihf-addr2line -fe pop-sot <pc> <lr>)\n",
            sig, uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr, si->si_addr,
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    fflush(stderr);
    fflush(stdout);
    _exit(128 + sig);
}

__attribute__((constructor)) static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
