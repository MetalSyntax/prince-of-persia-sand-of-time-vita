/* reSOT Vita: caché de recomp_lookup en lugar de la tabla plana de upstream.
 *
 * recomp_dispatch_init() del C generado reserva una entrada por byte de código
 * guest: 0x75D10A × 4 B = 30 MB para ~14 400 funciones. En Android el calloc
 * deja sin comprometer lo no tocado; en la Vita sale del heap fijo de newlib y
 * se paga entero (medido 2026-10-21: 42 MB de 48 en uso en el menú, 30 de la
 * tabla). main_vita.c ya no la construye y las llamadas indirectas pasan por
 * aquí (-Wl,--wrap=recomp_lookup): caché directa de 256 KB delante de la
 * búsqueda binaria original. Los NULL no se guardan (los resuelve la tabla
 * del kernel). Cada entrada es va | fn<<32 en 64 bits atómicos (ldrexd en
 * ARMv7): un hilo nunca ve la va de una entrada con la función de otra. */
#include <stdint.h>

typedef void (*recomp_func_t)(void);
recomp_func_t __real_recomp_lookup(uint32_t xbox_va);

#define CACHE_BITS 15
static uint64_t s_cache[1u << CACHE_BITS];

recomp_func_t __wrap_recomp_lookup(uint32_t xbox_va)
{
    uint64_t *slot = &s_cache[(xbox_va * 2654435761u) >> (32 - CACHE_BITS)];
    uint64_t e = __atomic_load_n(slot, __ATOMIC_RELAXED);
    recomp_func_t fn;
    if ((uint32_t)e == xbox_va && (e >> 32))
        return (recomp_func_t)(uintptr_t)(e >> 32);
    fn = __real_recomp_lookup(xbox_va);
    if (fn)
        __atomic_store_n(slot, (uint64_t)xbox_va | ((uint64_t)(uintptr_t)fn << 32), __ATOMIC_RELAXED);
    return fn;
}
