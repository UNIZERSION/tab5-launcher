#pragma once
#include <stdint.h>
typedef struct {
    uint32_t magic_word, secure_version, reserv1[2];
    char version[32], project_name[32], time[16], date[16], idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full, max_efuse_blk_rev_full;
    uint8_t mmu_page_size, reserv3[3];
    uint32_t reserv2[18];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
