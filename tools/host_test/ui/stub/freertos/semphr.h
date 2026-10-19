#pragma once
#include "FreeRTOS.h"
/* one LVGL thread in the snapshots: the locks always succeed */
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
#define xSemaphoreCreateMutex() ((SemaphoreHandle_t)1)
#define xSemaphoreTake(s, t) ((void)(s), (void)(t), pdPASS)
#define xSemaphoreGive(s) ((void)(s), pdPASS)
