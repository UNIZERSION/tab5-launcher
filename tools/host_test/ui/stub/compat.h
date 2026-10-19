/* forced include: newlib functions the launcher uses that MinGW lacks */
#pragma once
#include <string.h>
#include <time.h>
#include "sdkconfig.h"
static inline size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}
static inline size_t strlcat(char *d, const char *s, size_t n)
{
    size_t l = strnlen(d, n);
    return l == n ? n + strlen(s) : l + strlcpy(d + l, s, n - l);
}
#ifdef _WIN32
#include <direct.h>
#include <dirent.h>
#define mkdir(p, m) _mkdir(p)
/* MinGW's struct dirent has no d_type: PMAN's .bin scan (SANITY CHECK, not in the snapshots) compiles, its folder
   test never matches */
#define d_type d_ino
#define DT_DIR 0x7fff
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#endif
