#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sclk_io_num;
    int mosi_io_num;
    int dc_io_num;
    int cs_io_num;
    int reset_io_num;
    int bl_io_num;
    int spi_host;
    int width;
    int height;
    int pclk_hz;
    int rotation;
} lcd_clock_config_t;

typedef struct lcd_clock_t lcd_clock_t;

lcd_clock_t *lcd_clock_create(const lcd_clock_config_t *config);
void lcd_clock_destroy(lcd_clock_t *lcd);
bool lcd_clock_begin(lcd_clock_t *lcd);
void lcd_clock_update_time(lcd_clock_t *lcd, int hour, int minute, int second);
void lcd_clock_set_brightness(lcd_clock_t *lcd, bool on);
void lcd_clock_draw_frame(lcd_clock_t *lcd);
void lcd_clock_flush(lcd_clock_t *lcd);

#ifdef __cplusplus
}
#endif
