#include <inttypes.h>
#include <stdio.h>

#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_clock.h"
#include "wifi_sta.h"

static const char *TAG = "MEM";

static void print_memory_status(void)
{
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = (psram_total > 0) ? heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0;
    size_t psram_used = (psram_total > 0) ? (psram_total - psram_free) : 0;

    size_t flash_total = 0;
#if CONFIG_ESPTOOLPY_FLASHSIZE_1MB
    flash_total = 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_2MB
    flash_total = 2 * 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_4MB
    flash_total = 4 * 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_8MB
    flash_total = 8 * 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_16MB
    flash_total = 16 * 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_32MB
    flash_total = 32 * 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_64MB
    flash_total = 64 * 1024 * 1024;
#elif CONFIG_ESPTOOLPY_FLASHSIZE_128MB
    flash_total = 128 * 1024 * 1024;
#endif

    const esp_partition_t *running = esp_ota_get_running_partition();
    size_t flash_used = 0;
    size_t flash_free = flash_total;

    if (running != nullptr) {
        flash_used = running->size;
        flash_free = (flash_total > flash_used) ? (flash_total - flash_used) : 0;
    }

    if (psram_total > 0) {
        float psram_ratio = 100.0f * (float)psram_used / (float)psram_total;
        ESP_LOGI(TAG,
                 "PSRAM: total=%" PRIu32 "B, used=%" PRIu32 "B, free=%" PRIu32 "B, usage=%.2f%%",
                 (uint32_t)psram_total,
                 (uint32_t)psram_used,
                 (uint32_t)psram_free,
                 psram_ratio);
    } else {
        ESP_LOGI(TAG, "PSRAM: not available");
    }

    if (flash_total > 0) {
        float flash_ratio = 100.0f * (float)flash_used / (float)flash_total;
        ESP_LOGI(TAG,
                 "Flash: total=%" PRIu32 "B, used=%" PRIu32 "B, free=%" PRIu32 "B, usage=%.2f%%",
                 (uint32_t)flash_total,
                 (uint32_t)flash_used,
                 (uint32_t)flash_free,
                 flash_ratio);
    } else {
        ESP_LOGI(TAG, "Flash: unavailable");
    }
}

extern "C" void app_main(void)
{
    print_memory_status();

    lcd_clock_config_t config = {0};
    config.sclk_io_num = GPIO_NUM_12;
    config.mosi_io_num = GPIO_NUM_11;
    config.dc_io_num = GPIO_NUM_13;
    config.cs_io_num = GPIO_NUM_10;
    config.reset_io_num = GPIO_NUM_9;
    config.bl_io_num = GPIO_NUM_14;
    config.spi_host = (spi_host_device_t)SPI2_HOST;
    config.width = 240;
    config.height = 320;
    config.pclk_hz = 20 * 1000 * 1000;

    lcd_clock_t *lcd = lcd_clock_create(&config);
    if (lcd == NULL || !lcd_clock_begin(lcd)) {
        ESP_LOGE(TAG, "LCD clock initialization failed");
        return;
    }

    lcd_clock_set_brightness(lcd, true);
    lcd_clock_update_time(lcd, 12, 34, 56);

    for (int i = 0; i < 10; ++i) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        lcd_clock_update_time(lcd, 12, 34, 56 + i);
    }

    lcd_clock_destroy(lcd);
}
