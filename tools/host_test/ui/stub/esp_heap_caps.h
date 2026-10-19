#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 8
#define heap_caps_malloc(s, c) malloc(s)
#define heap_caps_calloc(n, s, c) calloc(n, s)
#define heap_caps_free(p) free(p)
