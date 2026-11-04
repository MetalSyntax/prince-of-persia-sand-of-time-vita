/* Prueba de compat/divmod_vita.c contra libgcc (mismo ABI ARM que la Vita):
 *
 *   port/vita/hosttest/run.sh divmod
 *
 * Compara cociente y resto de int64/uint64 en casos límite y 2 millones de pares aleatorios
 * (la mitad con operandos de 32 bits, que es el atajo). Imprime "divmod OK". */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Las dos rutas: la envuelta (lo que usa el juego) y __real_* (libgcc, por --wrap). */
typedef struct { int64_t q, r; } sres;
typedef struct { uint64_t q, r; } ures;
/* __value_in_regs no existe en GCC: se llama por asm para leer r0-r3. */
static sres call_s(const char *fn, int64_t n, int64_t d);
static ures call_u(const char *fn, uint64_t n, uint64_t d);

#define CALL4(sym, n, d, out)                                                            \
    do {                                                                                 \
        register uint32_t r0 __asm__("r0") = (uint32_t)(n), r1 __asm__("r1") = (uint32_t)((n) >> 32); \
        register uint32_t r2 __asm__("r2") = (uint32_t)(d), r3 __asm__("r3") = (uint32_t)((d) >> 32); \
        __asm__ volatile("bl " sym : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "ip", "lr", "cc", "memory"); \
        (out).q = ((uint64_t)r1 << 32) | r0;                                             \
        (out).r = ((uint64_t)r3 << 32) | r2;                                             \
    } while (0)

static sres call_s(const char *fn, int64_t n, int64_t d)
{
    sres o;
    if (fn[0] == 'w') CALL4("__wrap___aeabi_ldivmod", n, d, o);
    else CALL4("__real___aeabi_ldivmod", n, d, o);
    return o;
}
static ures call_u(const char *fn, uint64_t n, uint64_t d)
{
    ures o;
    if (fn[0] == 'w') CALL4("__wrap___aeabi_uldivmod", n, d, o);
    else CALL4("__real___aeabi_uldivmod", n, d, o);
    return o;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int check(int64_t n, int64_t d)
{
    sres a = call_s("w", n, d), b = call_s("r", n, d);
    ures c = call_u("w", (uint64_t)n, (uint64_t)d), e = call_u("r", (uint64_t)n, (uint64_t)d);
    if (a.q != b.q || a.r != b.r) { printf("ldivmod %lld / %lld: %lld %lld != %lld %lld\n",
        (long long)n, (long long)d, (long long)a.q, (long long)a.r, (long long)b.q, (long long)b.r); return 1; }
    if (c.q != e.q || c.r != e.r) { printf("uldivmod %llu / %llu\n", (unsigned long long)n, (unsigned long long)d); return 1; }
    return 0;
}

int main(void)
{
    static const int64_t edge[] = { 0, 1, -1, 2, -2, 7, -7, INT32_MAX, INT32_MIN, (int64_t)INT32_MAX + 1,
        (int64_t)INT32_MIN - 1, UINT32_MAX, (int64_t)UINT32_MAX + 1, INT64_MAX, INT64_MIN, 0x80000000ll };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof edge / sizeof *edge; i++)
        for (unsigned j = 0; j < sizeof edge / sizeof *edge; j++)
            if (edge[j] && !(edge[i] == INT64_MIN && edge[j] == -1)) bad += check(edge[i], edge[j]);
    for (unsigned i = 0; i < 2000000 && bad < 10; i++) {
        int64_t n = (int64_t)next(), d = (int64_t)next();
        if (i & 1) { n = (int32_t)n; d = (int32_t)(d >> (next() & 31)); }   /* el atajo, divisores pequeños */
        if (d && !(n == INT64_MIN && d == -1)) bad += check(n, d);
    }
    puts(bad ? "divmod FALLA" : "divmod OK");
    return bad != 0;
}
