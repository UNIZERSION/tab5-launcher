#pragma once
#include <stdint.h>
typedef struct {
    int model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;
void esp_chip_info(esp_chip_info_t *out);
