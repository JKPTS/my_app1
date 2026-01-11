// ===== FILE: main/display_uart.c =====
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "driver/uart.h"

#include "config_store.h"
#include "display_uart.h"

static const char *TAG = "DISP_UART";

// UART to CYD
// ESP32-S3 TX GPIO21 -> CYD RX GPIO3 (or whatever RX you choose on CYD)
#define DISP_UART_NUM        UART_NUM_2
#define DISP_UART_BAUD       115200
#define DISP_UART_TX_PIN     21

// IMPORTANT: uart_driver_install() requires rx_buf_size > 0 even if you don't use RX.
#define DISP_UART_RX_BUF     256
#define DISP_UART_TX_BUF     0

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_pending = false;

static void sanitize_commas(char *s)
{
    if (!s) return;
    for (; *s; s++) {
        if (*s == ',') *s = ' ';
        if (*s == '\n' || *s == '\r') *s = ' ';
    }
}

static void build_msg(char *out, size_t out_len)
{
    if (!out || out_len < 32) return;
    out[0] = 0;

    const foot_config_t *cfg = config_store_get();
    if (!cfg) {
        // 1 bank + 8 switches placeholder
        snprintf(out, out_len, "@U,0,NA,NA,NA,NA,NA,NA,NA,NA\r\n");
        return;
    }

    int bank = (int)config_store_get_current_bank();
    if (bank < 0) bank = 0;
    if (bank >= (int)cfg->bank_count) bank = 0;

    char bn[NAME_LEN];
    strncpy(bn, cfg->bank_name[bank], sizeof(bn));
    bn[NAME_LEN - 1] = 0;
    sanitize_commas(bn);

    int pos = snprintf(out, out_len, "@U,%d,%s", bank, bn);

    for (int k = 0; k < NUM_BTNS; k++) {
        char sn[NAME_LEN];
        strncpy(sn, cfg->switch_name[bank][k], sizeof(sn));
        sn[NAME_LEN - 1] = 0;
        sanitize_commas(sn);

        pos += snprintf(out + pos,
                        (pos < (int)out_len) ? out_len - (size_t)pos : 0,
                        ",%s", sn);
    }

    snprintf(out + pos,
             (pos < (int)out_len) ? out_len - (size_t)pos : 0,
             "\r\n");
}

static void disp_task(void *arg)
{
    (void)arg;
    char msg[256];

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // debounce + coalesce
        vTaskDelay(pdMS_TO_TICKS(60));
        while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pending = false;
        if (s_lock) xSemaphoreGive(s_lock);

        build_msg(msg, sizeof(msg));

        int w = uart_write_bytes(DISP_UART_NUM, msg, (int)strlen(msg));
        uart_wait_tx_done(DISP_UART_NUM, pdMS_TO_TICKS(50));
        ESP_LOGD(TAG, "tx %d bytes", w);
    }
}

void display_uart_init(void)
{
    if (s_task) return;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate = DISP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(DISP_UART_NUM, &cfg));

    // TX only: set TX pin, leave RX as no change
    ESP_ERROR_CHECK(uart_set_pin(DISP_UART_NUM,
                                 DISP_UART_TX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_driver_install(DISP_UART_NUM,
                                        DISP_UART_RX_BUF,
                                        DISP_UART_TX_BUF,
                                        0,
                                        NULL,
                                        0));

    xTaskCreatePinnedToCore(disp_task, "disp_uart", 3072, NULL, 6, &s_task, 0);

    // send once on boot
    display_uart_request_refresh();

    ESP_LOGI(TAG, "display uart ready (UART%d TX GPIO%d, %d bps)",
             (int)DISP_UART_NUM, (int)DISP_UART_TX_PIN, (int)DISP_UART_BAUD);
}

void display_uart_request_refresh(void)
{
    if (!s_task) return;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_pending) {
        s_pending = true;
        xTaskNotifyGive(s_task);
    } else {
        // still notify to coalesce bursts
        xTaskNotifyGive(s_task);
    }
    if (s_lock) xSemaphoreGive(s_lock);
}
