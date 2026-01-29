// ===== FILE: main/rgb_led.c =====

#include "rgb_led.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"

#include "led_strip.h"
#include "led_strip_rmt.h"

static const char *TAG = "RGBLED";

static led_strip_handle_t s_strip = NULL;
static uint8_t  s_brightness_percent = 100;
static uint32_t s_hex[RGB_LED_STRIP_LED_COUNT];
static uint8_t  s_on[RGB_LED_STRIP_LED_COUNT];

static inline uint8_t apply_brightness_u8(uint8_t v)
{
    // brightness 0..100
    return (uint8_t)((uint32_t)v * (uint32_t)s_brightness_percent / 100u);
}

static void apply_all(void)
{
    if (!s_strip) return;

    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) {
        if (s_on[i]) {
            uint32_t hex = s_hex[i] & 0xFFFFFFu;
            uint8_t r = (uint8_t)((hex >> 16) & 0xFF);
            uint8_t g = (uint8_t)((hex >>  8) & 0xFF);
            uint8_t b = (uint8_t)((hex >>  0) & 0xFF);

            r = apply_brightness_u8(r);
            g = apply_brightness_u8(g);
            b = apply_brightness_u8(b);

            // led_strip uses RGB arguments; pixel_format handles GRB on wire
            led_strip_set_pixel(s_strip, i, r, g, b);
        } else {
            led_strip_set_pixel(s_strip, i, 0, 0, 0);
        }
    }
    esp_err_t r = led_strip_refresh(s_strip);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "led_strip_refresh failed: %s", esp_err_to_name(r));
    }
}

esp_err_t rgb_led_init(void)
{
    if (s_strip) return ESP_OK;

    memset(s_on, 0, sizeof(s_on));
    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_hex[i] = 0x000000;
    s_brightness_percent = 100;

    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_STRIP_GPIO,
        .max_leds = RGB_LED_STRIP_LED_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        // Conservative defaults for broad ESP32-S3 compatibility.
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    (void)led_strip_clear(s_strip);
    (void)led_strip_refresh(s_strip);

    ESP_LOGI(TAG, "ws2812 init ok (gpio=%d leds=%d)", (int)RGB_LED_STRIP_GPIO, (int)RGB_LED_STRIP_LED_COUNT);
    return ESP_OK;
}

void rgb_led_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (s_brightness_percent == percent) return;
    s_brightness_percent = percent;
    apply_all();
}

uint8_t rgb_led_get_brightness(void)
{
    return s_brightness_percent;
}

void rgb_led_set_pixel_hex(int idx, uint32_t hex_rgb)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return;
    uint32_t v = hex_rgb & 0xFFFFFFu;
    if (s_hex[idx] == v) return;
    s_hex[idx] = v;
    apply_all();
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
    for (int i = 0; i < n; i++) s_hex[i] = hex_rgb[i] & 0xFFFFFFu;
    apply_all();
}

void rgb_led_set_hex(uint32_t hex_rgb)
{
    uint32_t v = hex_rgb & 0xFFFFFFu;
    for (int i = 0; i < RGB_LED_STRIP_LED_COUNT; i++) s_hex[i] = v;
    apply_all();
}

uint32_t rgb_led_get_hex(void)
{
    return s_hex[0] & 0xFFFFFFu;
}

void rgb_led_set_pixel_on(int idx, int on)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return;
    uint8_t v = on ? 1u : 0u;
    if (s_on[idx] == v) return;
    s_on[idx] = v;
    apply_all();
}

int rgb_led_get_pixel_on(int idx)
{
    if (idx < 0 || idx >= RGB_LED_STRIP_LED_COUNT) return 0;
    return s_on[idx] ? 1 : 0;
}

void rgb_led_all_off(void)
{
    memset(s_on, 0, sizeof(s_on));
    if (!s_strip) return;
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
}

void rgb_led_all_on(void)
{
    // If driver not initialized yet, nothing to do.
    if (!s_strip) return;
    memset(s_on, 1, sizeof(s_on));
    apply_all();
}
