/* reSOT Vita: registro de hilos ([THR] en el log) y afinidad a los 3 núcleos.
 *
 * Primera prueba en consola (2026-10-21, logs/log_001.log): 30 fps de pantalla
 * pero el juego parado tras cargar soundx0.big; el ejecutor NV2A con "0 ms
 * executing" (en Vita3K, 12 s de cada 15) e "[IRQL] ... held by tid 1004 for
 * >100 ms". Hipótesis: la Vita no reparte tiempo entre hilos de igual
 * prioridad en un mismo núcleo y un hilo que espera girando deja sin CPU al
 * ejecutor; en Vita3K cada hilo es uno del Mac y no pasa.
 *
 * Todo hilo del runtime pasa por aquí (-Wl,--wrap=pthread_create y
 * --wrap=CreateThread): se registra con su rutina de inicio y, salvo
 * RECOMP_VITA_AFFINITY=0, se le da SCE_KERNEL_CPU_MASK_USER_ALL.
 * vita_threads_log() imprime por hilo prioridad, núcleo, máscara y % de CPU
 * desde la llamada anterior: la evidencia para el siguiente paso. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "platform/win32_compat.h"

#define MAX_THREADS 64

typedef struct {
    SceUID thid;
    void *entry;               /* rutina de inicio (addr2line sobre el ELF) */
    unsigned long long last_us;
} ThreadSlot;

static ThreadSlot s_threads[MAX_THREADS];
static volatile int s_count;
static int s_affinity = -1;

void vita_thread_register(void *entry)
{
    int i = __atomic_fetch_add(&s_count, 1, __ATOMIC_RELAXED);
    if (i >= MAX_THREADS) return;
    s_threads[i].entry = entry;
    s_threads[i].thid = sceKernelGetThreadId();
    if (s_affinity < 0) s_affinity = !(getenv("RECOMP_VITA_AFFINITY") && !strcmp(getenv("RECOMP_VITA_AFFINITY"), "0"));
    if (s_affinity) sceKernelChangeThreadCpuAffinityMask(s_threads[i].thid, SCE_KERNEL_CPU_MASK_USER_ALL);
}

typedef struct { void *(*fn)(void *); void *arg; } PtBox;

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

static void tls_register(void);
static void tls_unregister(void);

static void *pt_tramp(void *p)
{
    PtBox b = *(PtBox *)p;
    free(p);
    tls_register();
    vita_thread_register((void *)b.fn);
    {
        void *r = b.fn(b.arg);
        tls_unregister();
        return r;
    }
}

int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    PtBox *b = (PtBox *)malloc(sizeof *b);
    int r;
    if (!b) return __real_pthread_create(th, attr, fn, arg);
    b->fn = fn; b->arg = arg;
    if ((r = __real_pthread_create(th, attr, pt_tramp, b)) != 0) free(b);
    return r;
}

/* CreateThread va por pthread_create con su trampolín; aquí se apunta la rutina real. */
typedef struct { LPTHREAD_START_ROUTINE fn; LPVOID arg; } CtBox;

HANDLE __real_CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

static DWORD WINAPI ct_tramp(LPVOID p)
{
    CtBox b = *(CtBox *)p;
    SceUID me = sceKernelGetThreadId();
    int i, n = s_count < MAX_THREADS ? s_count : MAX_THREADS;
    free(p);
    for (i = n - 1; i >= 0; i--)
        if (s_threads[i].thid == me) { s_threads[i].entry = (void *)b.fn; break; }
    return b.fn(b.arg);
}

HANDLE __wrap_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg,
                           DWORD flags, LPDWORD tid)
{
    CtBox *b = (CtBox *)malloc(sizeof *b);
    HANDLE h;
    if (!b) return __real_CreateThread(sa, stack, fn, arg, flags, tid);
    b->fn = fn; b->arg = arg;
    if (!(h = __real_CreateThread(sa, stack, ct_tramp, b, flags, tid))) free(b);
    return h;
}

void vita_threads_log(unsigned interval_ms)
{
    int i, n = s_count < MAX_THREADS ? s_count : MAX_THREADS;
    for (i = 0; i < n; i++) {
        SceKernelThreadInfo info;
        unsigned long long us;
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetThreadInfo(s_threads[i].thid, &info) < 0) continue;   /* terminó */
        us = info.runClocks;   /* µs */
        fprintf(stderr, "[THR] %-24.24s entry=%p prio=%d cpu=%d mask=0x%X status=0x%X cpu%%=%u\n",
                info.name, s_threads[i].entry, info.currentPriority, info.currentCpuId,
                (unsigned)info.currentCpuAffinityMask, (unsigned)info.status,
                (unsigned)((us - s_threads[i].last_us) / (interval_ms * 10ull)));
        s_threads[i].last_us = us;
    }
}

/* Reservas grandes del heap de newlib ([BIG], con el límite del log por etiqueta): log_011
 * lo mostró lleno (pico 48,9 MB de 48) y creciendo ~300 KB cada 10 s en la partida. */
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__real_memalign(size_t, size_t);
static volatile unsigned s_big_kb;

static void *big(const char *what, size_t n, void *lr, void *p)
{
    if (n >= (256u << 10)) {
        unsigned total = __atomic_add_fetch(&s_big_kb, (unsigned)(n >> 10), __ATOMIC_RELAXED);
        fprintf(stderr, "[BIG] %s %u KB lr=%p -> %p (total %u KB)\n", what, (unsigned)(n >> 10), lr, p, total);
    }
    return p;
}
void *__wrap_malloc(size_t n) { return big("malloc", n, __builtin_return_address(0), __real_malloc(n)); }
void *__wrap_calloc(size_t a, size_t b) { return big("calloc", a * b, __builtin_return_address(0), __real_calloc(a, b)); }
void *__wrap_realloc(void *o, size_t n) { return big("realloc", n, __builtin_return_address(0), __real_realloc(o, n)); }
void *__wrap_memalign(size_t a, size_t n) { return big("memalign", n, __builtin_return_address(0), __real_memalign(a, n)); }

/* ── TLS rápido (plan de optimización, fase 1) ────────────────────────────────
 * Cada acceso a una variable __thread (los registros guest g_eax..., 110 354 sitios
 * del binario) llamaba a __emutls_get_address: __gthread_active_p ->
 * pthread_getspecific -> pte_osTlsGetValue -> vitasdk_get_tls_data ->
 * __getreent_for_thread -> sceKernelGetTLSAddr y un recorrido de lista. Aquí cada
 * hilo registrado guarda las direcciones ya resueltas, indexadas por el índice de
 * emutls. El hilo se identifica por TPIDRURO (mrc, legible en modo usuario; no se
 * escribe nada en él ni en ningún slot de TLS). Si TPIDRURO vale 0 o lo comparten
 * dos hilos vivos, todo vuelve a la ruta original. */
#define TLS_SLOTS 2048
#define TLS_TAB   256
typedef struct { uint32_t tid; SceUID thid; void *addr[TLS_SLOTS]; } TlsRec;
typedef struct { size_t size, align; uintptr_t index; void *templ; } EmutlsObj;   /* libgcc emutls */
void *__real___emutls_get_address(EmutlsObj *obj);
static TlsRec *volatile s_tls[TLS_TAB];
static volatile int s_tls_off;

static inline uint32_t tpidruro(void)
{
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

void *__wrap___emutls_get_address(EmutlsObj *obj)
{
    uint32_t t = tpidruro(), i, k;
    if (!s_tls_off) {
        for (k = 0; k < 4; k++) {
            TlsRec *r = s_tls[((t >> 4) + k) & (TLS_TAB - 1)];
            if (r && r->tid == t) {
                if ((i = (uint32_t)obj->index) != 0 && i < TLS_SLOTS) {
                    void *a = r->addr[i];
                    if (!a) r->addr[i] = a = __real___emutls_get_address(obj);
                    return a;
                }
                break;
            }
        }
    }
    return __real___emutls_get_address(obj);
}

/* Al empezar cada hilo (pt_tramp) y en el principal. */
static void tls_register(void)
{
    uint32_t t = tpidruro(), k, slot = TLS_TAB;
    SceUID me = sceKernelGetThreadId();
    TlsRec *r;
    if (s_tls_off) return;
    if (!t) { s_tls_off = 1; fprintf(stderr, "[TLS] TPIDRURO = 0: TLS rápido desactivado\n"); return; }
    for (k = 0; k < 4; k++) {
        uint32_t j = ((t >> 4) + k) & (TLS_TAB - 1);
        TlsRec *o = s_tls[j];
        if (o && o->tid == t && o->thid != me) {
            SceKernelThreadInfo info;
            memset(&info, 0, sizeof info);
            info.size = sizeof info;
            if (sceKernelGetThreadInfo(o->thid, &info) >= 0 && info.status != 0x10 /* DORMANT */) {
                s_tls_off = 1;
                fprintf(stderr, "[TLS] TPIDRURO 0x%08X compartido por dos hilos: TLS rápido desactivado\n", t);
                return;
            }
        }
        if (o && o->tid == t) { slot = j; break; }   /* hilo muerto con el mismo valor: se reemplaza */
        if (!o && slot == TLS_TAB) slot = j;
    }
    if (slot == TLS_TAB || !(r = (TlsRec *)calloc(1, sizeof *r))) return;   /* sin hueco: ruta original */
    r->tid = t; r->thid = me;
    s_tls[slot] = r;
}

/* Al terminar un hilo: su registro deja de valer (su TLS se libera). */
static void tls_unregister(void)
{
    uint32_t t = tpidruro(), k;
    for (k = 0; k < 4; k++) {
        uint32_t j = ((t >> 4) + k) & (TLS_TAB - 1);
        TlsRec *o = s_tls[j];
        if (o && o->tid == t && o->thid == sceKernelGetThreadId()) { s_tls[j] = NULL; return; }
    }
}

/* Para el hilo principal y los creados con sceKernelCreateThread (audio, log, vídeo). */
void vita_tls_register_main(void) { tls_register(); }
void vita_tls_register(void) { tls_register(); }
void vita_tls_unregister(void) { tls_unregister(); }
