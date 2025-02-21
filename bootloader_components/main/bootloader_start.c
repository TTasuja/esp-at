/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>
#include <strings.h>
#include "esp_log.h"

#include "bootloader_init.h"
#include "bootloader_common.h"
#include "bootloader_utility.h"
#include "esp_rom_gpio.h"
#include "soc/uart_periph.h"
#include "hal/wdt_hal.h"
#include "hal/wdt_types.h"
#include "hal/uart_ll.h"

#define NOP_CYCLES 40000   // Number of NOP cycles for approximately 1 millisecond, needs tuning
static volatile uint32_t uptime_ms = 0;


#if defined(CONFIG_BOOTLOADER_COMPRESSED_ENABLED)
#include "bootloader_custom_ota.h"
#endif

static const char *TAG = "boot";

void wdt_feed();
int uart_read(void *buf, int size);
uint32_t timer_uptime_ms();
void delay_nop(uint32_t delay_ms);

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    bootloader_state_t bs = {0};
    int boot_index = 2; //TTA was INVALID_INDEX
    char buffer[32];
    uint32_t t;
    int l;

    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK)
        bootloader_reset();

    //timer_init();

    t = timer_uptime_ms() + 100;
    while (timer_uptime_ms() < t)
{
        wdt_feed();
	delay_nop(1);  // Delay 1 ms
}

    esp_rom_gpio_connect_in_signal(20, UART_PERIPH_SIGNAL(1, SOC_UART_RX_PIN_IDX), 0);
    esp_rom_gpio_connect_out_signal(21, UART_PERIPH_SIGNAL(1, SOC_UART_TX_PIN_IDX), 0, 0);
    esp_rom_gpio_connect_in_signal(6, UART_PERIPH_SIGNAL(0, SOC_UART_RX_PIN_IDX), 0);
    esp_rom_gpio_connect_out_signal(7, UART_PERIPH_SIGNAL(0, SOC_UART_TX_PIN_IDX), 0, 0);

    t = timer_uptime_ms() + 2000;
    l = 0;

    while (timer_uptime_ms() < t) {
        wdt_feed();
	delay_nop(1);  // Delay 1 ms
        if (uart_read(&buffer[l], 1) == 1) {
            if ((buffer[l] == '\r') || (buffer[l] == '\n')) {
                if (l) {
                    buffer[l] = 0;
                    ESP_LOGE(TAG, "UART: %s", buffer);
                    if (!strcasecmp(buffer, "AT+FACTORYBOOT")) {
                        ESP_LOGI(TAG, "FACTORY BOOT!!!");
                        boot_index = FACTORY_INDEX;
                        break;
                    }

                    l = 0;
                }
            } else {
                if (l < (sizeof(buffer) - 1))
                    l++;
            }
        }
    }

    t = timer_uptime_ms() + 100;
    while (timer_uptime_ms() < t)
{
        wdt_feed();
delay_nop(1);  // Delay 1 ms
}
    esp_rom_gpio_connect_in_signal(20, UART_PERIPH_SIGNAL(1, SOC_UART_RX_PIN_IDX), 0);
    esp_rom_gpio_connect_out_signal(21, UART_PERIPH_SIGNAL(1, SOC_UART_TX_PIN_IDX), 0, 0);
    esp_rom_gpio_connect_in_signal(6, UART_PERIPH_SIGNAL(0, SOC_UART_RX_PIN_IDX), 0);
    esp_rom_gpio_connect_out_signal(7, UART_PERIPH_SIGNAL(0, SOC_UART_TX_PIN_IDX), 0, 0);

    if (bootloader_utility_load_partition_table(&bs)) {
        if (boot_index == INVALID_INDEX)
            boot_index = bootloader_utility_get_selected_boot_partition(&bs);
    } else {
        ESP_LOGE(TAG, "load partition table error!");
        boot_index = INVALID_INDEX;
    }

    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

#if defined(CONFIG_BOOTLOADER_COMPRESSED_ENABLED)
    // 2.1 Call custom OTA routine
    boot_index = bootloader_custom_ota_main(&bs, boot_index);
#endif

    ESP_LOGI(TAG, "boot index: %d", boot_index);
    bootloader_utility_load_boot_image(&bs, boot_index);
}

// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}

void wdt_feed()
{       
    wdt_hal_context_t rtc_wdt_ctx = {
        .inst = WDT_RWDT,
        .rwdt_dev = &LP_WDT // ESP32-C6 uses LP_WDT instead of RTCCNTL
    };

    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_feed(&rtc_wdt_ctx);
    wdt_hal_write_protect_enable(&rtc_wdt_ctx);
}

int uart_read(void *buf, int size)
{
    int l;

    l = uart_ll_get_rxfifo_len(UART_LL_GET_HW(0));
    if (l > size)
        l = size;

    if (l)
        uart_ll_read_rxfifo(UART_LL_GET_HW(0), buf, l);

    return l;
}


/*
void timer_init()
{
    timer_ll_enable_clock(TIMER_LL_GET_HW(0), 0, true);
    timer_ll_set_clock_source(TIMER_LL_GET_HW(0), 0, SYSTIMER_CLK_SRC_XTAL);
    timer_ll_set_clock_prescale(TIMER_LL_GET_HW(0), 0, (XTAL_FREQ_HZ  / 10000) - 1);
    timer_ll_enable_auto_reload(TIMER_LL_GET_HW(0), 0, false);
    timer_ll_set_count_direction(TIMER_LL_GET_HW(0), 0, GPTIMER_COUNT_UP);
    timer_ll_enable_counter(TIMER_LL_GET_HW(0), 0, true);
}

uint32_t timer_uptime_ms()
{
    return timer_ll_get_counter_value(TIMER_LL_GET_HW(0), 0) / 10;
}
*/

void delay_nop(uint32_t delay_ms)
{
    for (uint32_t i = 0; i < delay_ms; i++) {
        // Loop and execute NOPs for each millisecond
        for (uint32_t j = 0; j < NOP_CYCLES; j++) {
            __asm__ volatile("nop");  // NOP instruction
        }
        uptime_ms++;  // Increment uptime every millisecond
    }
}

uint32_t timer_uptime_ms()
{
    return uptime_ms;
}
