/* reSOT (Vita): newlib no trae <sys/sysinfo.h>. El runtime lo usa para la
 * memoria total del host (getsysinfo). En Vita la respuesta se deriva del
 * bloque del port, así que el campo que mira (totalram) vale lo que la
 * memman_vita.c expone por getpagesize(). */
#ifndef RESOT_VITA_SYS_SYSINFO_H
#define RESOT_VITA_SYS_SYSINFO_H
#include <sys/types.h>
#include <sys/param.h>

struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
};

int sysinfo(struct sysinfo *info);
long sysconf(int name);
#endif
