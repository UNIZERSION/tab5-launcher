#pragma once
#include "FreeRTOS.h"
/* the snapshots run a worker to its end right away; tasks that wait forever (the config saver, the OTA worker)
   are refused, and their callers write / fetch without them */
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define xTaskNotifyGive(t) ((void)(t), pdPASS)
#define ulTaskNotifyTake(clear, wait) ((void)(clear), (void)(wait), 0u)
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio,
                                   TaskHandle_t *handle, int core);
void vTaskDelete(TaskHandle_t t);
