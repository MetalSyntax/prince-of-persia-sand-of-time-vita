/* reSOT (Vita): lo POSIX que el runtime usa y la newlib de VitaSDK declara
 * pero no implementa (o no trae). */
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/sysmem.h>

#include "sys/sysinfo.h"

int posix_memalign(void **out, size_t align, size_t size)
{
    void *p = memalign(align, size);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}

/* struct tm en UTC -> time_t (días desde 1970 por la fórmula civil de Howard Hinnant). */
time_t timegm(struct tm *tm)
{
    long long y = tm->tm_year + 1900LL, m = tm->tm_mon + 1, era, yoe, doy, doe, days;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

int sysinfo(struct sysinfo *info)
{
    SceKernelFreeMemorySizeInfo fm = { sizeof fm };
    if (!info) return -1;
    memset(info, 0, sizeof *info);
    sceKernelGetFreeMemorySize(&fm);
    info->totalram = 512u * 1024 * 1024;
    info->freeram  = (unsigned long)fm.size_user;
    info->mem_unit = 1;
    return 0;
}

/* Lo que pide kernel_file.c (búsqueda en carpetas de guardado): '*', '?' y
 * FNM_CASEFOLD. ponytail: sin clases '[...]' ni FNM_PATHNAME; ampliar si un
 * patrón del juego las usa (sale en el log como "no match"). */
int fnmatch(const char *pattern, const char *string, int flags)
{
    int fold = flags & FNM_CASEFOLD;
    for (; *pattern; pattern++, string++) {
        if (*pattern == '*') {
            while (pattern[1] == '*') pattern++;
            if (!pattern[1]) return 0;
            for (; *string; string++)
                if (fnmatch(pattern + 1, string, flags) == 0) return 0;
            return FNM_NOMATCH;
        }
        if (!*string) return FNM_NOMATCH;
        if (*pattern == '?') continue;
        if (fold ? tolower((unsigned char)*pattern) != tolower((unsigned char)*string)
                 : *pattern != *string)
            return FNM_NOMATCH;
    }
    return *string ? FNM_NOMATCH : 0;
}
