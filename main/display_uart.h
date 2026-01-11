// ===== FILE: main/display_uart.h =====
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Init UART TX-only link to CYD (ESP32-2432S028R)
void display_uart_init(void);

// Request a refresh push (bank + switch names). Safe to call from other modules.
void display_uart_request_refresh(void);

#ifdef __cplusplus
} // extern "C"
#endif
