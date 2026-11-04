/* reSOT Vita: MMU por software para la memoria guest.
 *
 * Upstream reserva 4 GB de direcciones y crea alias con mmap (espejos de RAM,
 * ventana contigua 0x80000000, apertura tiled 0xF0000000). La Vita es de 32
 * bits y no tiene mmap, así que las direcciones "host" que calcula el runtime
 * (H + va, con vuelta en 32 bits) son en parte ficticias. Esta tabla las
 * resuelve: host_real = f + vvm_pt[f >> 12], por páginas de 4 KB.
 *
 * Lo que es memoria real queda con desplazamiento 0: un bloque de 112 MB que
 * cubre de una vez NV2A, thunks, MCPX, flash y la RAM (guest 0xFD000000..
 * 0x04000000 con la vuelta). El resto (ventana contigua, tiled, espejos,
 * reservas altas) apunta por tabla a su almacenamiento.
 *
 * vitaize.py hace que XBOX_PTR() del código generado y los accesos crudos del
 * runtime pasen por vvm_ptr(). Ver PORTING_PLAN.md, Fase 3R.
 */
#ifndef RESOT_VITA_VVM_H
#define RESOT_VITA_VVM_H
#include <stddef.h>
#include <stdint.h>

#define VVM_PAGE_SHIFT 12
#define VVM_PAGES      (1u << (32 - VVM_PAGE_SHIFT))

extern int32_t vvm_pt[VVM_PAGES];

/* Atajo (plan de optimización, fase 3): la RAM del juego (64 MB desde vvm_ram_lo) es la
 * vista identidad, desplazamiento 0, sin leer la tabla. vvm_fastpath_enable() lo activa
 * solo si comprueba que toda esa zona de la tabla vale 0; mientras, vvm_ram_size = 0. */
extern uint32_t vvm_ram_lo, vvm_ram_size;
static inline void *vvm_ptr(uint32_t host)
{
    if (__builtin_expect(host - vvm_ram_lo < vvm_ram_size, 1)) return (void *)(uintptr_t)host;
    return (void *)(uintptr_t)(host + (uint32_t)vvm_pt[host >> VVM_PAGE_SHIFT]);
}
/* XBOX_PTR del código generado (C4 de PERF_PLAN.md): con el atajo activo y la RAM guest en
 * g_xbox_mem_offset, un va < vvm_gen_lim es base + va (3 instrucciones). Las dos se leen con un asm
 * sin memoria: GCC las carga una vez por función en vez de tras cada escritura volatile (antes, ~7
 * instrucciones por acceso: un 40% del código generado). Se fijan en vvm_fastpath_enable, antes de
 * que corra código del juego, y no cambian; con vvm_gen_lim = 0 todo va por vvm_ptr. */
extern uint32_t vvm_gen_base, vvm_gen_lim;
static inline uint32_t vvm_fixed(const uint32_t *p)
{
#if defined(__arm__)
    uint32_t v;
    __asm__("ldr %0, [%1]" : "=r"(v) : "r"(p));
    return v;
#else
    return *p;
#endif
}
extern ptrdiff_t g_xbox_mem_offset;
static inline uintptr_t vvm_gen_ptr(uint32_t va)
{
    if (__builtin_expect(va < vvm_fixed(&vvm_gen_lim), 1)) return (uintptr_t)(va + vvm_fixed(&vvm_gen_base));
    return (uintptr_t)vvm_ptr(va + (uint32_t)g_xbox_mem_offset);   /* como antes (también antes del atajo) */
}

/* Tras xbox_MemoryLayoutInit: 1 si el atajo quedó activo. */
int vvm_fastpath_enable(void);

/* Reserva el bloque real (antes de xbox_MemoryLayoutInit). 1 = OK. */
int vvm_init(void);
/* Bytes de almacenamiento real pedidos hasta ahora (para el log). */
size_t vvm_committed_bytes(void);

#endif
