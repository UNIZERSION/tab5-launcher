/* Host stubs for l5_fw.c and l5_inst.c (OTA): app description, image check, timer, slots registry. */
#pragma once
#include "stubs.h"
#include <strings.h>

#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_IMAGE_INVALID 0x2002
#define CONFIG_IDF_FIRMWARE_CHIP_ID 0x0012
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
#define MALLOC_CAP_SPIRAM 8
#define heap_caps_calloc(n, s, c) calloc(n, s)

static inline size_t strlcat(char *d, const char *s, size_t n)
{
    size_t l = strlen(d);
    return l + strlcpy(d + l, s, n > l ? n - l : 0);
}

typedef struct {
    uint32_t magic_word, secure_version, reserv1[2];
    char version[32], project_name[32], time[16], date[16], idf_ver[32];
    uint8_t app_elf_sha256[32];
    uint16_t min_efuse_blk_rev_full, max_efuse_blk_rev_full;
    uint8_t mmu_page_size, reserv3[3];
    uint32_t reserv2[18];
} esp_app_desc_t;

typedef enum { ESP_IMAGE_VERIFY, ESP_IMAGE_VERIFY_SILENT, ESP_IMAGE_LOAD } esp_image_load_mode_t;
typedef struct {
    uint32_t start_addr;
    uint32_t image_len;
} esp_image_metadata_t;
esp_err_t esp_image_verify(esp_image_load_mode_t mode, const esp_partition_pos_t *part, esp_image_metadata_t *data);
int64_t esp_timer_get_time(void);

typedef struct {
    const char *name, *author, *src, *fid, *ver;
} l5_slot_reg_t;
esp_err_t l5_slots_register(int index, const uint8_t elf_sha[32], uint32_t image_len, const l5_slot_reg_t *r);
void l5_slots_release(int index);
bool l5_chip_rev_ok(uint16_t rev_min_full, uint16_t rev_max_full);
const char *l5_chip_name(uint16_t chip_id);
