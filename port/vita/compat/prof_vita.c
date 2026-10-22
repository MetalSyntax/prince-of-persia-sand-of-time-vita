/* reSOT Vita: perfil por muestreo del ejecutor NV2A ([PROF] en el log).
 *
 * log_015: el ejecutor trabaja 6-10 s de cada 10 y lo medido en el backend
 * (draws, texturas, compose) solo explica ~80 de los ~250 ms de cada fotograma; el
 * resto está en upstream (pushbuffer, índices, constantes...). nv2a_pb_exec.c (con
 * los .inc del backend) se compila con -finstrument-functions: cada entrada/salida
 * de función apila/desapila un puntero (sin leer el reloj). Un hilo de alta
 * prioridad mira la pila cada 1 ms y cuenta la función en curso (propio) y todas
 * las de la pila (inclusivo). vita_prof_log() imprime el top cada 10 s; las
 * direcciones se simbolizan con addr2line sobre build/vita-port/pop-sot.elf.
 * La pila es global: lo instrumentado corre casi siempre en el hilo del ejecutor. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#define NI __attribute__((no_instrument_function))
#define DEPTH 256
#define SLOTS 1024

static void *volatile s_stack[DEPTH];
static volatile int s_depth;
static struct { void *fn; uint32_t self, incl; } s_tab[SLOTS];
static uint32_t s_samples, s_idle;

void __cyg_profile_func_enter(void *fn, void *site) NI;
void __cyg_profile_func_exit(void *fn, void *site) NI;

void __cyg_profile_func_enter(void *fn, void *site)
{
    int d = s_depth;
    (void)site;
    if (d < DEPTH) s_stack[d] = fn;
    s_depth = d + 1;
}

void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn; (void)site;
    if (s_depth > 0) s_depth--;
}

static NI uint32_t *slot(void *fn, int incl)
{
    uint32_t h = ((uint32_t)(uintptr_t)fn >> 2) * 2654435761u >> 22, i;
    for (i = 0; i < 16; i++) {
        uint32_t k = (h + i) & (SLOTS - 1);
        if (s_tab[k].fn == fn || !s_tab[k].fn) {
            s_tab[k].fn = fn;
            return incl ? &s_tab[k].incl : &s_tab[k].self;
        }
    }
    return NULL;
}

static NI int sampler(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        int d = s_depth, i, j;
        void *seen[32];
        int nseen = 0;
        uint32_t *c;
        sceKernelDelayThread(1000);
        s_samples++;
        if (d <= 0 || d > DEPTH) { s_idle++; continue; }
        if ((c = slot(s_stack[d - 1], 0)) != NULL) (*c)++;
        for (i = d - 1; i >= 0 && nseen < 32; i--) {   /* inclusivo: cada función una vez */
            void *fn = s_stack[i];
            for (j = 0; j < nseen && seen[j] != fn; j++) ;
            if (j < nseen) continue;
            seen[nseen++] = fn;
            if ((c = slot(fn, 1)) != NULL) (*c)++;
        }
    }
    return 0;
}

NI void vita_prof_start(void)
{
    SceUID t = sceKernelCreateThread("pop-prof", sampler, 0x10000100 - 32, 0x4000, 0, 0, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}

static NI void top(int incl)
{
    int i, k, best;
    uint32_t used[12];
    for (k = 0; k < 12; k++) {
        uint32_t v = 0;
        best = -1;
        for (i = 0; i < SLOTS; i++) {
            uint32_t x = incl ? s_tab[i].incl : s_tab[i].self;
            int dup = 0, j;
            for (j = 0; j < k; j++) dup |= used[j] == (uint32_t)i;
            if (s_tab[i].fn && !dup && x > v) { v = x; best = i; }
        }
        if (best < 0 || !v) break;
        used[k] = (uint32_t)best;
        fprintf(stderr, "[PROF] %s %4.1f%% %p\n", incl ? "incl" : "self",
                100.0 * v / (s_samples ? s_samples : 1), s_tab[best].fn);
    }
}

/* Cada 10 s desde el hilo principal: top propio e inclusivo, y reinicio. */
NI void vita_prof_log(void)
{
    if (!s_samples) return;
    fprintf(stderr, "[PROF] %u muestras, ejecutor fuera de código instrumentado %.0f%%\n",
            s_samples, 100.0 * s_idle / s_samples);
    top(0);
    top(1);
    memset(s_tab, 0, sizeof s_tab);
    s_samples = s_idle = 0;
}
