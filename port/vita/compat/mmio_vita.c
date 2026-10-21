/* reSOT Vita: camino lento de compat/mmio_vita.h (registros de APU y AC'97). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vvm.h"
#include "mmio_vita.h"

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern ptrdiff_t g_xbox_mem_offset;
uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

#define AC97_RR 0x02u

static uint8_t *guest(uint32_t va) { return vvm_ptr(va + (uint32_t)g_xbox_mem_offset); }

uint32_t vita_mmio_read(uint32_t va, unsigned size)
{
    uint32_t v = 0;
    if (g_apu_state)
        return (uint32_t)mcpx_apu_mmio_read(g_apu_state, va - MMIO_APU_BASE, size);
    memcpy(&v, guest(va), size);   /* sin APU (RECOMP_AC97_READY=0): memoria normal */
    return v;
}

void vita_mmio_write(uint32_t va, uint32_t value, unsigned size)
{
    static int logged;
    uint32_t off;
    if (mmio_apu(va) && g_apu_state) {
        if (!logged++) fprintf(stderr, "[MMIO] primera escritura APU: +0x%05X = %08X\n",
                               va - MMIO_APU_BASE, value);
        mcpx_apu_mmio_write(g_apu_state, va - MMIO_APU_BASE, value, size);
        return;
    }
    memcpy(guest(va), &value, size);
    if (va - MMIO_AC97_BASE >= MMIO_AC97_SIZE) return;
    /* El reset de canal (RR) se completa al instante, como ac97_clear_reset_bits()
     * de xbox_memory_layout.c: barre el byte de control de cada canal del NABM. */
    for (off = 0x10B; off < 0x180; off += 0x10) {
        uint8_t *r = guest(MMIO_AC97_BASE + off);
        if (*r & AC97_RR) *r = (uint8_t)(*r & ~AC97_RR);
    }
}
