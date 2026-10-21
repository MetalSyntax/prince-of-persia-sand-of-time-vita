/* reSOT Vita: registros de la APU y del bus master AC'97 sin trampas de página.
 *
 * Upstream deja esas páginas sin acceso y emula la instrucción que falla
 * (VEH en Windows, SIGSEGV en Android). La Vita no tiene ninguna de las dos,
 * así que tools/vitaize.py reescribe los MEMn() enteros del C generado de
 * DirectSound (el único código que toca esos registros) por estos accesores:
 * la RAM sigue igual y solo el rango de la APU va al modelo de upstream.
 * Mismos rangos que xbox_memory_layout.c: registros + VP de la APU (los
 * DSP GP/EP quedan como RAM) y la página AC'97 cuyas escrituras completan
 * el reset de canal.
 */
#ifndef RESOT_VITA_MMIO_H
#define RESOT_VITA_MMIO_H
#include <stdint.h>

#define MMIO_APU_BASE  0xFE800000u
#define MMIO_APU_SIZE  0x00030000u
#define MMIO_AC97_BASE 0xFEC00000u
#define MMIO_AC97_SIZE 0x00001000u

uint32_t vita_mmio_read(uint32_t va, unsigned size);
void     vita_mmio_write(uint32_t va, uint32_t value, unsigned size);

static inline int mmio_apu(uint32_t a) { return a - MMIO_APU_BASE < MMIO_APU_SIZE; }
static inline int mmio_any(uint32_t a) { return mmio_apu(a) || a - MMIO_AC97_BASE < MMIO_AC97_SIZE; }

#ifdef MEM32   /* solo en el C generado (recomp_types.h) */
static inline uint32_t mmio_r8(uint32_t a)  { return mmio_apu(a) ? (uint8_t)vita_mmio_read(a, 1) : MEM8(a); }
static inline uint32_t mmio_r16(uint32_t a) { return mmio_apu(a) ? (uint16_t)vita_mmio_read(a, 2) : MEM16(a); }
static inline uint32_t mmio_r32(uint32_t a) { return mmio_apu(a) ? vita_mmio_read(a, 4) : MEM32(a); }
static inline void mmio_w8(uint32_t a, uint32_t v)  { if (mmio_any(a)) vita_mmio_write(a, (uint8_t)v, 1); else MEM8(a) = (uint8_t)v; }
static inline void mmio_w16(uint32_t a, uint32_t v) { if (mmio_any(a)) vita_mmio_write(a, (uint16_t)v, 2); else MEM16(a) = (uint16_t)v; }
static inline void mmio_w32(uint32_t a, uint32_t v) { if (mmio_any(a)) vita_mmio_write(a, v, 4); else MEM32(a) = v; }

/* Los tipos de MEMn(): uint8/16 se promueven igual que antes en las expresiones. */
#define MMIO_R8(a)     ((uint8_t)mmio_r8((uint32_t)(a)))
#define MMIO_R16(a)    ((uint16_t)mmio_r16((uint32_t)(a)))
#define MMIO_R32(a)    mmio_r32((uint32_t)(a))
#define MMIO_W8(a, v)  mmio_w8((uint32_t)(a), (uint32_t)(v))
#define MMIO_W16(a, v) mmio_w16((uint32_t)(a), (uint32_t)(v))
#define MMIO_W32(a, v) mmio_w32((uint32_t)(a), (uint32_t)(v))
#endif

#endif
