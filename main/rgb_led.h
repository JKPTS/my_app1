// ===== FILE: main/rgb_led.h =====
#pragma once

#include <stdint.h>
#include "esp_err.h"

// WS2812 / NeoPixel LED driver (ESP-IDF led_strip + RMT)
//
// - single chain: 8 leds on one data pin
// - brightness api: 0..100 (output capped to 90%)
// - periodic refresh every 1s re-applies stored colors without
//   forcing leds that are off to turn on

#ifndef RGB_LED_STRIP_LED_COUNT
#define RGB_LED_STRIP_LED_COUNT 8
#endif

#ifndef RGB_LED_GPIO
#define RGB_LED_GPIO 11
#endif

#ifndef RGB_LED_REFRESH_PERIOD_MS
#define RGB_LED_REFRESH_PERIOD_MS 1000
#endif

// init ws2812 strip (safe to call multiple times)
esp_err_t rgb_led_init(void);

// brightness percent 0..100 (applied on top of rgb values)
void     rgb_led_set_brightness(uint8_t percent);
uint8_t  rgb_led_get_brightness(void);

// per led color (0xrrggbb)
void     rgb_led_set_pixel_hex(int idx, uint32_t hex_rgb);
uint32_t rgb_led_get_pixel_hex(int idx);

// bulk set colors (hex_rgb length n, n<=RGB_LED_STRIP_LED_COUNT)
void     rgb_led_set_pixels_hex(const uint32_t *hex_rgb, int n);

// compatibility: set all leds to the same color (0xrrggbb)
void     rgb_led_set_hex(uint32_t hex_rgb);

// compatibility: get "global" color (returns led0)
uint32_t rgb_led_get_hex(void);

// per led on/off
void     rgb_led_set_pixel_on(int idx, int on);
int      rgb_led_get_pixel_on(int idx);

// turn all leds off (black). does not change stored colors.
void     rgb_led_all_off(void);

// turn all leds on (uses stored per-led colors)
void     rgb_led_all_on(void);
