#include "l5_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "l5_config";

l5_config_t g_cfg;

/* data.js colors */
const l5_color_t l5_colors[] = {
    {"red", 0xe3171f}, {"orange", 0xff5f1a}, {"amber", 0xf2b400}, {"green", 0x18b25a},
    {"cyan", 0x0fb4c8}, {"blue", 0x2f6bff}, {"violet", 0x8a4dff}, {"magenta", 0xe0218a},
};
const int l5_color_count = sizeof(l5_colors) / sizeof(l5_colors[0]);

static const uint16_t DIMS[] = {0, 10, 20, 30, 60, 120, 300}; /* L5.DIMS */
static const uint16_t OFFS[] = {0, 60, 120, 300, 600, 1800};   /* L5.OFFS */

void l5_config_defaults(l5_config_t *cfg)
{
    /* app.js defaults().cfg */
    *cfg = (l5_config_t){
        .bright = 70, .dim = 30, .screenOff = 120, .color = "red", .theme = "graphite", .rot = 0, .askSpiffs = true, .onlyBins = true,
        .bootTime = 3, .wifiAuto = true, .deepSleepStart = true, .btnOnlyStart = false, .idleClock = true,
        .scheme = "default",
    };
}

uint32_t l5_config_accent_hex(const l5_config_t *cfg)
{
    for (int i = 0; i < l5_color_count; i++) {
        if (strcmp(cfg->color, l5_colors[i].id) == 0) {
            return l5_colors[i].hex;
        }
    }
    return l5_colors[0].hex;
}

/* /config.conf is read, changed and written back by the settings saver and by OTA favorites */
static SemaphoreHandle_t file_lock(void)
{
    static SemaphoreHandle_t s_lock;
    static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
    if (!s_lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&s_mux);
        if (!s_lock) {
            s_lock = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (m) vSemaphoreDelete(m);
    }
    return s_lock;
}

/* the whole file as JSON, NULL when missing or not an object */
static cJSON *read_root(void)
{
    FILE *f = fopen(L5_CONFIG_PATH, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 && size <= 64 * 1024 ? malloc(size + 1) : NULL;
    cJSON *root = NULL;
    if (text) {
        text[fread(text, 1, size, f)] = 0;
        root = cJSON_Parse(text);
        free(text);
    }
    fclose(f);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t write_root(cJSON *root)
{
    char *text = cJSON_Print(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(L5_CONFIG_PATH, "wb");
    if (!f) {
        cJSON_free(text);
        return ESP_FAIL;
    }
    bool ok = fputs(text, f) >= 0;
    ok &= fclose(f) == 0;
    cJSON_free(text);
    return ok ? ESP_OK : ESP_FAIL;
}

static void set_item(cJSON *root, const char *key, cJSON *item)
{
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddItemToObject(root, key, item);
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void get_bool(const cJSON *root, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
    }
}

static bool get_int(const cJSON *root, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valueint;
    return true;
}

static void get_str(const cJSON *root, const char *key, char *out, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(v) && v->valuestring && strlen(v->valuestring) < len) {
        strcpy(out, v->valuestring);
    }
}

esp_err_t l5_config_load(l5_config_t *cfg)
{
    l5_config_defaults(cfg);
    FILE *f = fopen(L5_CONFIG_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "%s not found, using defaults", L5_CONFIG_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    fclose(f);
    cJSON *root = read_root();
    if (!root) {
        ESP_LOGE(TAG, "%s is not a JSON object, using defaults", L5_CONFIG_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    int v;
    if (get_int(root, "bright", &v)) {
        cfg->bright = clampi((v + 2) / 5 * 5, 5, 100); /* L5.setBright: 5% steps */
    }
    if (get_int(root, "dim", &v)) {
        cfg->dim = DIMS[0];
        for (size_t i = 0; i < sizeof(DIMS) / sizeof(DIMS[0]); i++) {
            if (v >= DIMS[i]) {
                cfg->dim = DIMS[i];
            }
        }
    }
    if (get_int(root, "screenOff", &v)) { /* files from before the setting keep the default 120 s */
        cfg->screenOff = OFFS[0];
        for (size_t i = 0; i < sizeof(OFFS) / sizeof(OFFS[0]); i++) {
            if (v >= OFFS[i]) {
                cfg->screenOff = OFFS[i];
            }
        }
    }
    if (get_int(root, "rot", &v)) {
        cfg->rot = v == 180 ? 180 : 0;
    }
    if (get_int(root, "bootTime", &v)) {
        cfg->bootTime = clampi(v, 1, 5);
    }
    get_str(root, "color", cfg->color, sizeof(cfg->color));
    get_str(root, "theme", cfg->theme, sizeof(cfg->theme));
    get_str(root, "scheme", cfg->scheme, sizeof(cfg->scheme));
    get_bool(root, "askSpiffs", &cfg->askSpiffs);
    get_bool(root, "onlyBins", &cfg->onlyBins);
    get_bool(root, "wifiAuto", &cfg->wifiAuto);
    get_bool(root, "deepSleepStart", &cfg->deepSleepStart);
    get_bool(root, "btnOnlyStart", &cfg->btnOnlyStart);
    get_bool(root, "idleClock", &cfg->idleClock);
    cJSON_Delete(root);

    bool known = strcmp(cfg->color, "app") == 0;
    for (int i = 0; i < l5_color_count; i++) {
        known |= strcmp(cfg->color, l5_colors[i].id) == 0;
    }
    if (!known) {
        strcpy(cfg->color, "red");
    }
    ESP_LOGI(TAG, "loaded %s: bright=%u dim=%u color=%s rot=%u bootTime=%u scheme=%s", L5_CONFIG_PATH,
             cfg->bright, cfg->dim, cfg->color, cfg->rot, cfg->bootTime, cfg->scheme);
    return ESP_OK;
}

esp_err_t l5_config_save(const l5_config_t *cfg)
{
    xSemaphoreTake(file_lock(), portMAX_DELAY);
    cJSON *root = read_root(); /* keys this firmware does not know yet (wuiUser, favorite, ...) stay */
    if (!root) {
        root = cJSON_CreateObject();
    }
    set_item(root, "bright", cJSON_CreateNumber(cfg->bright));
    set_item(root, "dim", cJSON_CreateNumber(cfg->dim));
    set_item(root, "screenOff", cJSON_CreateNumber(cfg->screenOff));
    set_item(root, "color", cJSON_CreateString(cfg->color));
    set_item(root, "theme", cJSON_CreateString(cfg->theme));
    set_item(root, "rot", cJSON_CreateNumber(cfg->rot));
    set_item(root, "askSpiffs", cJSON_CreateBool(cfg->askSpiffs));
    set_item(root, "onlyBins", cJSON_CreateBool(cfg->onlyBins));
    set_item(root, "bootTime", cJSON_CreateNumber(cfg->bootTime));
    set_item(root, "wifiAuto", cJSON_CreateBool(cfg->wifiAuto));
    set_item(root, "deepSleepStart", cJSON_CreateBool(cfg->deepSleepStart));
    set_item(root, "btnOnlyStart", cJSON_CreateBool(cfg->btnOnlyStart));
    set_item(root, "idleClock", cJSON_CreateBool(cfg->idleClock));
    set_item(root, "scheme", cJSON_CreateString(cfg->scheme));
    esp_err_t err = write_root(root);
    cJSON_Delete(root);
    xSemaphoreGive(file_lock());
    return err;
}

esp_err_t l5_config_remove(void)
{
    xSemaphoreTake(file_lock(), portMAX_DELAY);
    esp_err_t err = remove(L5_CONFIG_PATH) == 0 ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(file_lock());
    return err;
}

/* The current file steps aside as config.conf.old first: when src cannot take its name, the old one gets its name
   back and nothing changed. Under the file lock, so no settings save writes in between. */
esp_err_t l5_config_replace(const char *src)
{
    xSemaphoreTake(file_lock(), portMAX_DELAY);
    remove(L5_CONFIG_OLD);
    bool had = rename(L5_CONFIG_PATH, L5_CONFIG_OLD) == 0;
    bool ok = rename(src, L5_CONFIG_PATH) == 0;
    if (had) {
        if (ok) {
            remove(L5_CONFIG_OLD);
        } else {
            rename(L5_CONFIG_OLD, L5_CONFIG_PATH);
        }
    }
    xSemaphoreGive(file_lock());
    ESP_LOGI(TAG, "%s put in place of %s%s", src, L5_CONFIG_PATH, ok ? "" : ": failed, nothing changed");
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---------- OTA favorites: Launcher's "favorite": [{name, fid, link}] ---------- */

esp_err_t l5_favs_load(l5_favs_t *out)
{
    out->n = 0;
    xSemaphoreTake(file_lock(), portMAX_DELAY);
    cJSON *root = read_root();
    xSemaphoreGive(file_lock());
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "favorite")) {
        if (out->n == L5_FAV_MAX) break;
        l5_fav_t *f = &out->item[out->n];
        memset(f, 0, sizeof(*f));
        get_str(it, "name", f->name, sizeof(f->name));
        get_str(it, "fid", f->fid, sizeof(f->fid));
        get_str(it, "link", f->link, sizeof(f->link));
        if (f->name[0] && (f->fid[0] || f->link[0])) out->n++;
    }
    cJSON_Delete(root);
    return root ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t l5_favs_save(const l5_favs_t *favs)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; arr && i < favs->n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", favs->item[i].name);
        cJSON_AddStringToObject(o, "fid", favs->item[i].fid);
        cJSON_AddStringToObject(o, "link", favs->item[i].link);
        cJSON_AddItemToArray(arr, o);
    }
    if (!arr) return ESP_ERR_NO_MEM;
    xSemaphoreTake(file_lock(), portMAX_DELAY);
    cJSON *root = read_root();
    if (!root) {
        root = cJSON_CreateObject();
    }
    set_item(root, "favorite", arr);
    esp_err_t err = write_root(root);
    cJSON_Delete(root);
    xSemaphoreGive(file_lock());
    return err;
}
