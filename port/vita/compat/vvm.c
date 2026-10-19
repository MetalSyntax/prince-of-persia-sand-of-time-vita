/* reSOT Vita: API de memoria virtual Win32 del runtime sobre la tabla de vvm.h.
 *
 * Sustituye a VirtualAlloc/VirtualFree/VirtualProtect y a las vistas de
 * archivo (CreateFileMapping/MapViewOfFileEx) de win32_compat.c, que en POSIX
 * se apoyan en mmap. Las de upstream se renombran con -D al compilar
 * win32_compat.c (CMakeLists.txt). La secuencia que sigue
 * xbox_MemoryLayoutInit y que esto reproduce:
 *
 *   CreateFileMapping(64 MB)          -> RAM: su almacenamiento es H, dentro de B
 *   VirtualAlloc(NULL, span, RESERVE) -> devuelve H (g_memory_offset = H)
 *   MapViewOfFileEx(RAM, H)           -> identidad
 *   MapViewOfFileEx(RAM, H+k*64MB)    -> espejos por tabla
 *   CreateFileMapping(64 MB) + vista en H+0x80000000 -> ventana contigua
 *   VirtualAlloc(H+0xFD000000...)     -> NV2A/MCPX/flash: ya reales en B
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/win32_compat.h"
#include "vvm.h"

void view_register(void *addr, size_t len);
size_t view_take(const void *addr);

#define PAGE      0x1000u
#define B_SIZE    0x07000000u   /* guest 0xFD000000 .. 0x04000000 (con vuelta) */
#define B_TO_H    0x03000000u   /* H = B + esto: guest 0 */
#define RAM_SIZE  0x04000000u
#define CONTIG_VA 0x80000000u

int32_t vvm_pt[VVM_PAGES];

static uint8_t  *s_block;            /* B */
static uint32_t  s_h;                /* H: host de guest 0 */
static uint8_t   s_trash[PAGE] __attribute__((aligned(PAGE)));
static uint8_t   s_state[VVM_PAGES]; /* 0 libre, 1 reservado, 2 comprometido */
static int       s_mappings, s_span_given;
static ptrdiff_t s_contig_offset;
static size_t    s_committed;

typedef struct { uint32_t magic; uint8_t *backing; size_t size; } vvm_mapping;
#define MAPPING_MAGIC 0x56564D4Du

static int in_block(uint32_t a, size_t len)
{
    return a >= (uint32_t)(uintptr_t)s_block && (uint64_t)a + len <= (uint64_t)(uintptr_t)s_block + B_SIZE;
}

static void map_range(uint32_t host, size_t len, const uint8_t *real, int state)
{
    uint32_t p;
    for (p = 0; p < len; p += PAGE) {
        uint32_t f = host + p;
        vvm_pt[f >> VVM_PAGE_SHIFT] = real ? (int32_t)((uint32_t)(uintptr_t)(real + p) - f) : 0;
        s_state[f >> VVM_PAGE_SHIFT] = (uint8_t)state;
    }
}

/* Sin almacenamiento: lecturas y escrituras van a una página de descarte
 * (no a memoria del host que casualmente esté en esa dirección). */
static void void_range(uint32_t host, size_t len, int state)
{
    uint32_t p;
    if (in_block(host, len)) { map_range(host, len, NULL, state); return; }
    for (p = 0; p < len; p += PAGE) {
        uint32_t f = host + p;
        vvm_pt[f >> VVM_PAGE_SHIFT] = (int32_t)((uint32_t)(uintptr_t)s_trash - (f & ~(PAGE - 1)));
        s_state[f >> VVM_PAGE_SHIFT] = (uint8_t)state;
    }
}

static uint8_t *alloc_zeroed(size_t len)
{
    uint8_t *p = memalign(PAGE, len);
    if (p) { memset(p, 0, len); s_committed += len; }
    return p;
}

int vvm_init(void)
{
    if (s_block) return 1;
    s_block = alloc_zeroed(B_SIZE);
    if (!s_block) {
        fprintf(stderr, "[VVM] no hay %u MB para el bloque guest\n", B_SIZE >> 20);
        return 0;
    }
    s_h = (uint32_t)(uintptr_t)s_block + B_TO_H;
    s_contig_offset = (ptrdiff_t)s_h;
    fprintf(stderr, "[VVM] bloque %u MB en %p, guest 0 en 0x%08X, tabla %u KB\n",
            B_SIZE >> 20, (void *)s_block, s_h, (unsigned)(sizeof vvm_pt >> 10));
    return 1;
}

size_t vvm_committed_bytes(void) { return s_committed; }

/* El ejecutor de pushbuffer, OHCI y el trazador leen "mem + va" con va en la
 * ventana contigua (dma_resolve() devuelve 0x80000000 + P): con este
 * desplazamiento esa suma cae en el almacenamiento real de la ventana. */
ptrdiff_t xbox_GetMemoryOffset(void) { return s_contig_offset; }

static SIZE_T page_round(SIZE_T n) { return (n + PAGE - 1) & ~(SIZE_T)(PAGE - 1); }

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    uint32_t a = (uint32_t)(uintptr_t)address, p;
    (void)protect;
    size = page_round(size);
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    if (!address) {
        /* La reserva del span guest (RAM + espejos): empieza en H. */
        if ((type & MEM_RESERVE) && !(type & MEM_COMMIT) && size >= RAM_SIZE && !s_span_given) {
            s_span_given = 1;
            void_range(s_h, size, 1);
            view_register((void *)(uintptr_t)s_h, size);
            return (LPVOID)(uintptr_t)s_h;
        }
        /* ponytail: cualquier otra reserva sin dirección se compromete entera. */
        address = alloc_zeroed(size);
        if (!address) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        view_register(address, size);
        return address;
    }

    if (in_block(a, size)) {
        map_range(a, size, NULL, (type & MEM_COMMIT) ? 2 : 1);
    } else if (type & MEM_COMMIT) {
        /* Compromete por tramos lo que no lo estaba; lo ya comprometido se conserva. */
        for (p = 0; p < size; ) {
            uint32_t run = 0;
            uint8_t *real;
            while (p + run < size && s_state[(a + p + run) >> VVM_PAGE_SHIFT] == 2) p += PAGE;
            while (p + run < size && s_state[(a + p + run) >> VVM_PAGE_SHIFT] != 2) run += PAGE;
            if (!run) break;
            if (!(real = alloc_zeroed(run))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
            map_range(a + p, run, real, 2);
            p += run;
        }
    } else {
        void_range(a, size, 1);
    }
    if (type & MEM_RESERVE) view_register(address, size);
    return address;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type)
{
    uint32_t a = (uint32_t)(uintptr_t)address;
    if (type & MEM_RELEASE) {
        size_t len = view_take(address);
        if (!size) size = len;
        if (!size) return FALSE;
    }
    size = page_round(size);
    /* ponytail: el almacenamiento de un tramo liberado no se devuelve al heap
     * (haría falta contar referencias por bloque); solo se desmapea. */
    void_range(a, size, (type & MEM_RELEASE) ? 0 : 1);
    return TRUE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    /* Sin protección por páginas: las trampas MMIO de upstream (APU, AC'97,
     * OHCI) no saltan en Vita. Pendiente V0.3 (ver PORTING_PLAN.md). */
    (void)address; (void)size; (void)newProtect;
    if (oldProtect) *oldProtect = PAGE_READWRITE;
    return TRUE;
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    vvm_mapping *m;
    (void)file; (void)sa; (void)protect; (void)name;
    if (maxSizeHigh || !maxSizeLow || !vvm_init()) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!(m = calloc(1, sizeof *m))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    m->magic = MAPPING_MAGIC;
    m->size = maxSizeLow;
    if (s_mappings++ == 0 && maxSizeLow <= RAM_SIZE) {
        m->backing = (uint8_t *)(uintptr_t)s_h;   /* la RAM: la primera que crea el layout */
    } else if (!(m->backing = alloc_zeroed(page_round(maxSizeLow)))) {
        free(m);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return (HANDLE)m;
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    vvm_mapping *m = (vvm_mapping *)mapping;
    uint32_t base = (uint32_t)(uintptr_t)baseAddr;
    SIZE_T len;
    (void)access;
    if (!m || m->magic != MAPPING_MAGIC || offHigh || offLow >= m->size) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    len = count ? count : m->size - offLow;
    if (!baseAddr) return m->backing + offLow;
    map_range(base, page_round(len), m->backing + offLow, 2);
    if (base - s_h == CONTIG_VA)
        s_contig_offset = (ptrdiff_t)(uint32_t)((uint32_t)(uintptr_t)(m->backing + offLow) - CONTIG_VA);
    view_register(baseAddr, len);
    return baseAddr;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    if (!len) return FALSE;
    void_range((uint32_t)(uintptr_t)baseAddr, page_round(len), 0);
    return TRUE;
}

/* Lo que el VirtualAlloc de upstream (renombrado y sin uso) aún enlaza. */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return (void *)-1;
}
int munmap(void *addr, size_t len) { (void)addr; (void)len; return -1; }
int mprotect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return -1; }
int memfd_create(const char *name, unsigned flags) { (void)name; (void)flags; return -1; }
long syscall(long number, ...) { (void)number; return -1; }
