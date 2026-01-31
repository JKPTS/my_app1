// ===== FILE: main/rgb_led.c =====

#include "rgb_led.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = "RGBLED";

// single ws2812 chain
static led_strip_handle_t s_strip = NULL;

// protect strip operations + shared state
static SemaphoreHandle_t s_lock = NULL;

// periodic refresh timer (re-send stored state every 1s)
static esp_timer_handle_t s_refresh_timer = NULL;

// brightness api exposed to web/config: 0..100
static uint8_t s_brightness_ui_percent = 100;
// output cap: 0..90
static uint8_t s_brightness_out_percent = 90;

// stored per-led color + on/off
static uint32_t s_hex[RGB_LED_STRIP_LED_COUNT];
static uint8_t  s_on[RGB_LED_STRIP_LED_COUNT];

// gamma correction table generated at runtime (sRGB-ish -> linear-ish)
static uint8_t s_gamma8[256];
static bool    s_gamma_ready = false;

static void gamma_init_once(void)
{
    if (s_gamma_ready) return;

    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        // typical LED gamma ~2.2
        float y = powf(x, 2.2f);
        int v = (int)lroundf(y * 255.0f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        s_gamma8[i] = (uint8_t)v;
    }
    s_gamma_ready = true;
}

static inline uint8_t apply_brightness_u8(uint8_t v_lin)
{
    return (uint8_t)((uint32_t)v_lin * (uint32_t)s_brightness_out_percent / 100u);
}

static inline void lock_take(void)
{
    if (!s_lock) return;
    (void)xSemaphoreTake(s_lock, portMAX_DELAY);
}

static inline void lock_give(void)
{
    if (!s_lock) return;
    (void)xSemaphoreGive(s_lock);
}

static void apply_one_locked(int idx)
{
    if (!s_strip) return;
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return;

    if (s_on[idx]) {
        uint32_t hex = s_hex[idx] & 0xFFFFFFu;
        uint8_t r = (uint8_t)((hex >> 16) & 0xFF);
        uint8_t g = (uint8_t)((hex >>  8) & 0xFF);
        uint8_t b = (uint8_t)((hex >>  0) & 0xFF);

        uint8_t r_lin = apply_brightness_u8(s_gamma8[r]);
        uint8_t g_lin = apply_brightness_u8(s_gamma8[g]);
        uint8_t b_lin = apply_brightness_u8(s_gamma8[b]);

        // GRB order handled by LED_STRIP_COLOR_COMPONENT_FMT_GRB
        led_strip_set_pixel(s_strip, idx, r_lin, g_lin, b_lin);
    } else {
        led_strip_set_pixel(s_strip, idx, 0, 0, 0);
    }
}

static void apply_all_locked(void)
{
    if (!s_strip) return;

    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) {
        apply_one_locked(i);
    }

    esp_err_t r = led_strip_refresh(s_strip);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(r));
    }
}

static void periodic_refresh_cb(void *arg)
{
    (void)arg;
    if (!s_strip || !s_lock) return;

    // don't block: if another update is happening, skip this tick
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    apply_all_locked();
    xSemaphoreGive(s_lock);
}

static esp_err_t start_refresh_timer_once(void)
{
    if (s_refresh_timer) return ESP_OK;

    const esp_timer_create_args_t targs = {
        .callback = &periodic_refresh_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "rgb_refresh_1s",
        .skip_unhandled_events = true,
    };

    esp_err_t e = esp_timer_create(&targs, &s_refresh_timer);
    if (e != ESP_OK) return e;

    // periodic in microseconds
    int64_t period_us = (int64_t)RGB_LED_REFRESH_PERIOD_MS * 1000LL;
    e = esp_timer_start_periodic(s_refresh_timer, period_us);
    if (e != ESP_OK) {
        esp_timer_delete(s_refresh_timer);
        s_refresh_timer = NULL;
        return e;
    }
    return ESP_OK;
}

esp_err_t rgb_led_init(void)
{
    if (s_strip) return ESP_OK;

    gamma_init_once();

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    // init defaults
    lock_take();
    memset(s_on, 0, sizeof(s_on));
    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_hex[i] = 0x000000u;

    s_brightness_ui_percent = 100;
    s_brightness_out_percent = 90; // UI 100% -> 90% output
    lock_give();

    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_STRIP_LED_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .mem_block_symbols = 128,
        .flags.with_dma = false,
    };

    esp_err_t e = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "init strip gpio=%d failed: %s", (int)RGB_LED_GPIO, esp_err_to_name(e));
        s_strip = NULL;
        return e;
    }

    (void)led_strip_clear(s_strip);
    (void)led_strip_refresh(s_strip);

    e = start_refresh_timer_once();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "start refresh timer failed: %s", esp_err_to_name(e));
        // keep running even if timer fails
    }

    ESP_LOGI(TAG, "ws2812 init ok (%d leds on gpio%d, refresh=%dms)",
             (int)RGB_LED_STRIP_LED_COUNT, (int)RGB_LED_GPIO, (int)RGB_LED_REFRESH_PERIOD_MS);

    return ESP_OK;
}

void rgb_led_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;

    lock_take();
    if (s_brightness_ui_percent == percent) {
        lock_give();
        return;
    }

    s_brightness_ui_percent = percent;
    // map UI 0..100 -> output 0..90 (rounded)
    s_brightness_out_percent = (uint8_t)((percent * 90u + 50u) / 100u);

    apply_all_locked();
    lock_give();
}

uint8_t rgb_led_get_brightness(void)
{
    return s_brightness_ui_percent;
}

void rgb_led_set_pixel_hex(int idx, uint32_t hex_rgb)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return;

    lock_take();
    uint32_t v = hex_rgb & 0xFFFFFFu;
    if (s_hex[idx] == v) {
        lock_give();
        return;
    }

    s_hex[idx] = v;
    apply_one_locked(idx);
    (void)led_strip_refresh(s_strip);
    lock_give();
}

uint32_t rgb_led_get_pixel_hex(int idx)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return 0;
    return s_hex[idx] & 0xFFFFFFu;
}

void rgb_led_set_pixels_hex(const uint32_t *hex_rgb, int n)
{
    if (!hex_rgb || n <= 0) return;
    if (n > RGB_LED_STRIP_LED_COUNT) n = RGB_LED_STRIP_LED_COUNT;

    lock_take();
    for (int i = 0; i < n; i++) s_hex[i] = hex_rgb[i] & 0xFFFFFFu;
    apply_all_locked();
    lock_give();
}

void rgb_led_set_hex(uint32_t hex_rgb)
{
    uint32_t v = hex_rgb & 0xFFFFFFu;

    lock_take();
    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_hex[i] = v;
    apply_all_locked();
    lock_give();
}

uint32_t rgb_led_get_hex(void)
{
    return s_hex[0] & 0xFFFFFFu;
}

void rgb_led_set_pixel_on(int idx, int on)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return;

    lock_take();
    uint8_t v = on ? 1u : 0u;
    if (s_on[idx] == v) {
        lock_give();
        return;
    }

    s_on[idx] = v;
    apply_one_locked(idx);
    (void)led_strip_refresh(s_strip);
    lock_give();
}

int rgb_led_get_pixel_on(int idx)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return 0;
    return s_on[idx] ? 1 : 0;
}

void rgb_led_all_off(void)
{
    lock_take();
    memset(s_on, 0, sizeof(s_on));
    apply_all_locked();
    lock_give();
}

void rgb_led_all_on(void)
{
    lock_take();
    memset(s_on, 1, sizeof(s_on));
    apply_all_locked();
    lock_give();
}
