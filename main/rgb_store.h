// ===== FILE: main/rgb_store.h =====
#pragma once

#include <stdint.h>
#include "esp_err.h"

// Stores per-pixel RGB colors (0xRRGGBB) in NVS and applies them via rgb_led.
// Note: ON/OFF state is controlled by your program (footswitch / logic). This module only stores COLORS.

esp_err_t rgb_store_init(void);

// Apply stored colors + current config_store brightness to rgb_led.
// (Does NOT change pixel ON/OFF state.)
void rgb_store_apply(void);

// Number of pixels (same as RGB_LED_STRIP_LED_COUNT)
int rgb_store_count(void);

// Get color for one pixel (0xRRGGBB)
uint32_t rgb_store_get_pixel_hex(int idx);

// Set color for one pixel, save to NVS, and apply.
esp_err_t rgb_store_set_pixel_hex(int idx, uint32_t hex_rgb);

// Set all pixel colors (array length n). Saves and applies.
esp_err_t rgb_store_set_all_hex(const uint32_t *hex_rgb, int n);

// Compatibility: get/set a "global" color (sets all pixels to same value)
uint32_t rgb_store_get_hex(void);
esp_err_t rgb_store_set_hex(uint32_t hex_rgb);
