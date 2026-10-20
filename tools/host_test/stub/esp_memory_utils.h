#pragma once
#include <stdbool.h>
/* on the PC every buffer counts as internal RAM: the installer writes flash without its bounce buffer */
static inline bool esp_ptr_internal(const void *p) { (void)p; return true; }
