#include "lcd_clock.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "lcd_clock";

typedef struct lcd_clock_t {
    lcd_clock_config_t config;
    bool initialized;
    int current_hour;
    int current_minute;
    int current_second;
    uint16_t *framebuffer;
    size_t framebuffer_size;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} lcd_clock_t;

static uint16_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t red = (r >> 3) & 0x1F;
    uint16_t green = (g >> 2) & 0x3F;
    uint16_t blue = (b >> 3) & 0x1F;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static void write_pixel(lcd_clock_t *lcd, int x, int y, uint16_t color) {
    if (lcd == NULL || lcd->framebuffer == NULL) {
        return;
    }
    if (x < 0 || y < 0 || x >= lcd->config.width || y >= lcd->config.height) {
        return;
    }
    lcd->framebuffer[(size_t)y * lcd->config.width + x] = color;
}

static void draw_rect(lcd_clock_t *lcd, int x, int y, int w, int h, uint16_t color) {
    if (lcd == NULL) {
        return;
    }
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            write_pixel(lcd, xx, yy, color);
        }
    }
}

static void draw_digit(lcd_clock_t *lcd, int x, int y, int value) {
    if (lcd == NULL) {
        return;
    }
    if (value < 0 || value > 9) {
        value = 0;
    }

    static const bool segments[10][7] = {
        {1, 1, 1, 0, 1, 1, 1},
        {0, 0, 1, 0, 0, 1, 0},
        {1, 0, 1, 1, 1, 0, 1},
        {1, 0, 1, 1, 0, 1, 1},
        {0, 1, 1, 1, 0, 1, 0},
        {1, 1, 0, 1, 0, 1, 1},
        {1, 1, 0, 1, 1, 1, 1},
        {1, 0, 1, 0, 0, 1, 0},
        {1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 0, 1, 1},
    };

    const int seg_w = 12;
    const int seg_h = 6;
    const int gap = 4;

    if (segments[value][0]) {
        draw_rect(lcd, x + 2, y, seg_w, seg_h, 0xFFFF);
    }
    if (segments[value][1]) {
        draw_rect(lcd, x, y + 8, seg_h, seg_w, 0xFFFF);
    }
    if (segments[value][2]) {
        draw_rect(lcd, x + seg_h + gap, y + 8, seg_h, seg_w, 0xFFFF);
    }
    if (segments[value][3]) {
        draw_rect(lcd, x + 2, y + 28, seg_w, seg_h, 0xFFFF);
    }
    if (segments[value][4]) {
        draw_rect(lcd, x, y + 36, seg_h, seg_w, 0xFFFF);
    }
    if (segments[value][5]) {
        draw_rect(lcd, x + seg_h + gap, y + 36, seg_h, seg_w, 0xFFFF);
    }
    if (segments[value][6]) {
        draw_rect(lcd, x + 2, y + 56, seg_w, seg_h, 0xFFFF);
    }
}

static void draw_two_digits(lcd_clock_t *lcd, int x, int y, int value) {
    if (lcd == NULL) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    draw_digit(lcd, x, y, (value / 10) % 10);
    draw_digit(lcd, x + 28, y, value % 10);
}

static void draw_colon(lcd_clock_t *lcd, int x, int y) {
    if (lcd == NULL) {
        return;
    }
    draw_rect(lcd, x, y + 10, 4, 4, 0xF800);
    draw_rect(lcd, x, y + 28, 4, 4, 0xF800);
}

static void clear_screen(lcd_clock_t *lcd, uint16_t color) {
    if (lcd == NULL || lcd->framebuffer == NULL) {
        return;
    }
    memset(lcd->framebuffer, 0, lcd->framebuffer_size);
    for (size_t i = 0; i < lcd->framebuffer_size / 2; ++i) {
        ((uint16_t *)lcd->framebuffer)[i] = color;
    }
}

lcd_clock_t *lcd_clock_create(const lcd_clock_config_t *config) {
    lcd_clock_t *lcd = (lcd_clock_t *)calloc(1, sizeof(lcd_clock_t));
    if (lcd == NULL) {
        return NULL;
    }
    if (config != NULL) {
        lcd->config = *config;
    }
    return lcd;
}

void lcd_clock_destroy(lcd_clock_t *lcd) {
    if (lcd == NULL) {
        return;
    }
    if (lcd->initialized) {
        esp_lcd_panel_disp_on_off(lcd->panel_handle, false);
        esp_lcd_panel_del(lcd->panel_handle);
        esp_lcd_panel_io_del(lcd->io_handle);
        spi_bus_free(lcd->config.spi_host);
        gpio_reset_pin((gpio_num_t)lcd->config.bl_io_num);
        gpio_reset_pin((gpio_num_t)lcd->config.reset_io_num);
        gpio_reset_pin((gpio_num_t)lcd->config.dc_io_num);
        gpio_reset_pin((gpio_num_t)lcd->config.cs_io_num);
    }
    free(lcd->framebuffer);
    free(lcd);
}

bool lcd_clock_begin(lcd_clock_t *lcd) {
    if (lcd == NULL || lcd->initialized) {
        return lcd != NULL && lcd->initialized;
    }

    lcd->framebuffer_size = (size_t)lcd->config.width * lcd->config.height * sizeof(uint16_t);
    lcd->framebuffer = (uint16_t *)calloc(1, lcd->framebuffer_size);
    if (lcd->framebuffer == NULL) {
        return false;
    }

    gpio_config_t io_conf = {0};
    io_conf.pin_bit_mask = (1ULL << lcd->config.bl_io_num) | (1ULL << lcd->config.reset_io_num) |
                           (1ULL << lcd->config.dc_io_num) | (1ULL << lcd->config.cs_io_num);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    gpio_set_level((gpio_num_t)lcd->config.bl_io_num, 0);
    gpio_set_level((gpio_num_t)lcd->config.reset_io_num, 1);
    gpio_set_level((gpio_num_t)lcd->config.dc_io_num, 0);
    gpio_set_level((gpio_num_t)lcd->config.cs_io_num, 1);

    spi_bus_config_t buscfg = {0};
    buscfg.sclk_io_num = (gpio_num_t)lcd->config.sclk_io_num;
    buscfg.mosi_io_num = (gpio_num_t)lcd->config.mosi_io_num;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;

    err = spi_bus_initialize((spi_host_device_t)lcd->config.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {0};
    io_config.dc_gpio_num = (gpio_num_t)lcd->config.dc_io_num;
    io_config.cs_gpio_num = (gpio_num_t)lcd->config.cs_io_num;
    io_config.pclk_hz = lcd->config.pclk_hz;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)lcd->config.spi_host, &io_config, &lcd->io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(err));
        spi_bus_free((spi_host_device_t)lcd->config.spi_host);
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {0};
    panel_config.reset_gpio_num = (gpio_num_t)lcd->config.reset_io_num;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;

    err = esp_lcd_new_panel_st7789(lcd->io_handle, &panel_config, &lcd->panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(lcd->io_handle);
        lcd->io_handle = NULL;
        spi_bus_free(lcd->config.spi_host);
        return false;
    }

    esp_lcd_panel_reset(lcd->panel_handle);
    esp_lcd_panel_init(lcd->panel_handle);
    esp_lcd_panel_invert_color(lcd->panel_handle, false);
    esp_lcd_panel_disp_on_off(lcd->panel_handle, true);
    lcd_clock_set_brightness(lcd, true);

    lcd->initialized = true;
    clear_screen(lcd, make_color(20, 20, 20));
    lcd_clock_flush(lcd);
    ESP_LOGI(TAG, "LCD clock initialized");
    return true;
}

void lcd_clock_update_time(lcd_clock_t *lcd, int hour, int minute, int second) {
    if (lcd == NULL || !lcd->initialized) {
        return;
    }
    lcd->current_hour = hour;
    lcd->current_minute = minute;
    lcd->current_second = second;
    lcd_clock_draw_frame(lcd);
}

void lcd_clock_set_brightness(lcd_clock_t *lcd, bool on) {
    if (lcd != NULL && lcd->initialized) {
        gpio_set_level((gpio_num_t)lcd->config.bl_io_num, on ? 1 : 0);
    }
}

void lcd_clock_draw_frame(lcd_clock_t *lcd) {
    if (lcd == NULL || lcd->framebuffer == NULL) {
        return;
    }
    clear_screen(lcd, make_color(15, 20, 30));
    draw_rect(lcd, 10, 10, lcd->config.width - 20, lcd->config.height - 20, make_color(40, 40, 40));
    draw_two_digits(lcd, 40, 80, lcd->current_hour);
    draw_colon(lcd, 112, 100);
    draw_two_digits(lcd, 128, 80, lcd->current_minute);
    draw_colon(lcd, 200, 100);
    draw_two_digits(lcd, 216, 80, lcd->current_second);
    lcd_clock_flush(lcd);
}

void lcd_clock_flush(lcd_clock_t *lcd) {
    if (lcd != NULL && lcd->initialized && lcd->panel_handle != NULL) {
        esp_lcd_panel_draw_bitmap(lcd->panel_handle, 0, 0, lcd->config.width - 1, lcd->config.height - 1, lcd->framebuffer);
    }
}
