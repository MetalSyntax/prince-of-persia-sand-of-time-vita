/* reSOT (Vita): newlib no trae <sys/mman.h>. win32_compat.c lo incluye para su
 * VirtualAlloc POSIX, que en Vita se renombra y no se usa: la memoria guest la
 * lleva compat/vvm.c. Aquí solo lo que hace falta para que compile. */
#ifndef RESOT_VITA_SYS_MMAN_H
#define RESOT_VITA_SYS_MMAN_H
#include <stddef.h>

#define PROT_NONE     0x0
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_NORESERVE 0x40
#define MAP_FAILED    ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   memfd_create(const char *name, unsigned flags);

#endif
