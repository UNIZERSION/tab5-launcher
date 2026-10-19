/* Host stubs for testing l5_ptable.c: simulated 16 MB flash with IDF-like write protection. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#define localtime_r(t, tm) (localtime_s((tm), (t)) ? NULL : (tm))
#endif
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

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_CRC 0x109
const char *esp_err_to_name(esp_err_t e);

#define ESP_LOGE(tag, fmt, ...) printf("  E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("  W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("  I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...)                 \
    do {                                                      \
        esp_err_t e_ = (x);                                   \
        if (e_ != ESP_OK) {                                   \
            ESP_LOGE(tag, fmt, ##__VA_ARGS__);                \
            return e_;                                        \
        }                                                     \
    } while (0)

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_DEFAULT 4
#define heap_caps_malloc(s, c) malloc(s)
#define heap_caps_free(p) free(p)

typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_TYPE_ANY = 0xff } esp_partition_type_t;
enum {
    ESP_PARTITION_SUBTYPE_APP_FACTORY = 0x00, ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 0x10, ESP_PARTITION_SUBTYPE_APP_OTA_MAX = 0x20,
    ESP_PARTITION_SUBTYPE_DATA_OTA = 0x00, ESP_PARTITION_SUBTYPE_DATA_PHY = 0x01, ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02,
    ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 0x03, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS = 0x04,
    ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM = 0x05, ESP_PARTITION_SUBTYPE_DATA_FAT = 0x81,
    ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82, ESP_PARTITION_SUBTYPE_ANY = 0xff,
};
typedef struct {
    uint32_t address, size, erase_size;
    esp_partition_type_t type;
    int subtype;
    char label[17];
    bool encrypted;
} esp_partition_t;
typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;
typedef struct {
    uint16_t magic;
    uint8_t type;
    uint8_t subtype;
    esp_partition_pos_t pos;
    uint8_t label[16];
    uint32_t flags;
} esp_partition_info_t;
#define ESP_PARTITION_MAGIC 0x50AA
#define ESP_PARTITION_MAGIC_MD5 0xEBEB
#define ESP_PARTITION_MD5_OFFSET 16
#define ESP_PARTITION_TABLE_MAX_ENTRIES 96
#define PART_TYPE_END 0xff
#define PART_SUBTYPE_END 0xff
esp_err_t esp_partition_table_verify(const esp_partition_info_t *t, bool log_errors, int *num);
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype, const char *label);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len);
const esp_partition_t *esp_ota_get_running_partition(void);

typedef struct esp_flash_t esp_flash_t;
extern esp_flash_t *esp_flash_default_chip;
esp_err_t esp_flash_read(esp_flash_t *c, void *dst, uint32_t addr, uint32_t len);
esp_err_t esp_flash_write(esp_flash_t *c, const void *src, uint32_t addr, uint32_t len);
esp_err_t esp_flash_erase_region(esp_flash_t *c, uint32_t addr, uint32_t len);
esp_err_t esp_flash_get_size(esp_flash_t *c, uint32_t *size);
esp_err_t esp_flash_set_dangerous_write_protection(esp_flash_t *c, bool protect);
bool esp_flash_encryption_enabled(void);

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;
    uint8_t buf[64];
} md5_context_t;
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
void esp_rom_md5_init(md5_context_t *c);
void esp_rom_md5_update(md5_context_t *c, const void *data, uint32_t len);
void esp_rom_md5_final(uint8_t *digest, md5_context_t *c);

typedef struct __attribute__((packed)) {
    uint8_t magic, segment_count, spi_mode, spi_speed_size;
    uint32_t entry_addr;
    uint8_t wp_pin;
    uint8_t spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t min_chip_rev;
    uint16_t min_chip_rev_full, max_chip_rev_full;
    uint8_t reserved[4];
    uint8_t hash_appended;
} esp_image_header_t;
typedef struct {
    uint32_t load_addr, data_len;
} esp_image_segment_header_t;
#define ESP_IMAGE_HEADER_MAGIC 0xE9
#define ESP_IMAGE_MAX_SEGMENTS 16

#define vTaskDelay(x) ((void)0)
#define pdMS_TO_TICKS(x) (x)

typedef struct {
    int unused;
} mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c) { (void)c; }
static inline int mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224) { (void)c; (void)is224; return 0; }
static inline int mbedtls_sha256_update(mbedtls_sha256_context *c, const uint8_t *d, size_t n) { (void)c; (void)d; (void)n; return 0; }
static inline int mbedtls_sha256_finish(mbedtls_sha256_context *c, uint8_t out[32]) { (void)c; memset(out, 0, 32); return 0; }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c) { (void)c; }

typedef int nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);

bool l5_board_sd_ready(void);
#define L5_MAX_SLOTS 8
void l5_slots_forget_all(void);
