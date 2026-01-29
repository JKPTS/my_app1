// ===== FILE: main/rgb_led.h =====
#pragma once

#include <stdint.h>
#include "esp_err.h"

// WS2812 / NeoPixel LED strip driver (RMT)
// - Designed for 8-pixel footswitch ring by default
// - Per-pixel color (0xRRGGBB) + per-pixel ON/OFF (OFF = black)
// - Brightness 0..100 scales RGB values before sending to strip

#ifndef RGB_LED_STRIP_GPIO
// Default GPIO for WS2812 data on many ESP32-S3 devkits is GPIO48.
// If you wired DIN to a different pin, change this value.
#define RGB_LED_STRIP_GPIO 11
#endif

#ifndef RGB_LED_STRIP_LED_COUNT
#define RGB_LED_STRIP_LED_COUNT 8
#endif

// Init WS2812 strip (safe to call multiple times)
esp_err_t rgb_led_init(void);

// Brightness percent 0..100 (applied on top of RGB values)
void     rgb_led_set_brightness(uint8_t percent);
uint8_t  rgb_led_get_brightness(void);

// ----- Per pixel color -----
void     rgb_led_set_pixel_hex(int idx, uint32_t hex_rgb);
uint32_t rgb_led_get_pixel_hex(int idx);

// Bulk set colors (hex_rgb length n, n<=RGB_LED_STRIP_LED_COUNT). Does NOT change ON/OFF state.
void     rgb_led_set_pixels_hex(const uint32_t *hex_rgb, int n);

// Compatibility: set all pixels to the same color (0xRRGGBB)
void     rgb_led_set_hex(uint32_t hex_rgb);

// Compatibility: get "global" color (returns pixel[0])
uint32_t rgb_led_get_hex(void);

// ----- Per pixel ON/OFF -----
void     rgb_led_set_pixel_on(int idx, int on);
int      rgb_led_get_pixel_on(int idx);

// Turn all pixels OFF (black). Does not change stored colors.
void     rgb_led_all_off(void);

// Turn all pixels ON (uses stored per-pixel colors)
void     rgb_led_all_on(void);
