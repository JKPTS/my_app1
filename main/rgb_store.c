// ===== FILE: main/rgb_store.c =====

#include "rgb_store.h"

#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_store.h"
#include "rgb_led.h"

static const char *TAG = "RGBSTORE";

#define RGB_NVS_NAMESPACE "cfg"
#define RGB_NVS_KEY_BLOB  "rgb_px"   // blob of uint32_t[RGB_LED_STRIP_LED_COUNT]

static uint32_t s_rgb_px[RGB_LED_STRIP_LED_COUNT];

static void set_default_colors(void)
{
    // default: all white
    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_rgb_px[i] = 0xFFFFFF;
}

static esp_err_t nvs_load_blob(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RGB_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK; // NVS not ready / namespace missing is fine

    size_t sz = 0;
    err = nvs_get_blob(h, RGB_NVS_KEY_BLOB, NULL, &sz);
    if (err == ESP_OK && sz == sizeof(s_rgb_px)) {
        err = nvs_get_blob(h, RGB_NVS_KEY_BLOB, s_rgb_px, &sz);
    } else {
        // key missing or wrong size -> defaults
        err = ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_blob(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RGB_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, RGB_NVS_KEY_BLOB, s_rgb_px, sizeof(s_rgb_px));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t rgb_store_init(void)
{
    // Ensure RGB LED driver is ready (safe to call multiple times)
    esp_err_t err = rgb_led_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rgb_led_init failed: %s", esp_err_to_name(err));
        return err;
    }

    set_default_colors();

    esp_err_t l = nvs_load_blob();
    if (l == ESP_OK) {
        for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_rgb_px[i] &= 0xFFFFFFu;
        ESP_LOGI(TAG, "loaded per-pixel colors (n=%d)", (int)RGB_LED_STRIP_LED_COUNT);
    } else {
        ESP_LOGI(TAG, "no saved per-pixel colors -> defaults");
    }

    return ESP_OK;
}

void rgb_store_apply(void)
{
    uint8_t br = config_store_get_led_brightness();
    rgb_led_set_brightness(br);
    rgb_led_set_pixels_hex(s_rgb_px, RGB_LED_STRIP_LED_COUNT);
}

int rgb_store_count(void)
{
    return RGB_LED_STRIP_LED_COUNT;
}

uint32_t rgb_store_get_pixel_hex(int idx)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return 0;
    return s_rgb_px[idx] & 0xFFFFFFu;
}

esp_err_t rgb_store_set_pixel_hex(int idx, uint32_t hex_rgb)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return ESP_ERR_INVALID_ARG;

    s_rgb_px[idx] = hex_rgb & 0xFFFFFFu;

    esp_err_t err = nvs_save_blob();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save per-pixel colors: %s", esp_err_to_name(err));
        // still apply in RAM
    }

    rgb_store_apply();
    return err;
}

esp_err_t rgb_store_set_all_hex(const uint32_t *hex_rgb, int n)
{
    if (!hex_rgb || n <= 0) return ESP_ERR_INVALID_ARG;
    if (n > RGB_LED_STRIP_LED_COUNT) n = RGB_LED_STRIP_LED_COUNT;

    for (int i = 0; i < n; i++) s_rgb_px[i] = hex_rgb[i] & 0xFFFFFFu;

    esp_err_t err = nvs_save_blob();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save per-pixel colors: %s", esp_err_to_name(err));
    }

    rgb_store_apply();
    return err;
}

uint32_t rgb_store_get_hex(void)
{
    return s_rgb_px[0] & 0xFFFFFFu;
}

esp_err_t rgb_store_set_hex(uint32_t hex_rgb)
{
    uint32_t v = hex_rgb & 0xFFFFFFu;
    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_rgb_px[i] = v;

    esp_err_t err = nvs_save_blob();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save per-pixel colors: %s", esp_err_to_name(err));
    }

    rgb_store_apply();
    return err;
}
