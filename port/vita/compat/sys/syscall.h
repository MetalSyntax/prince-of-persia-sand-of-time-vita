/* reSOT (Vita): win32_compat.c (rama __ANDROID__) llama a memfd_create por
 * syscall en su VirtualAlloc POSIX, que en Vita está renombrado y sin uso
 * (compat/vvm.c). Solo para que compile; syscall() falla siempre. */
#ifndef RESOT_VITA_SYS_SYSCALL_H
#define RESOT_VITA_SYS_SYSCALL_H
#define __NR_memfd_create 0
long syscall(long number, ...);
#endif
