#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

namespace lcd_clock {

class LcdClock {
public:
    struct Config {
        gpio_num_t sclk_io_num = GPIO_NUM_12;
        gpio_num_t mosi_io_num = GPIO_NUM_11;
        gpio_num_t dc_io_num = GPIO_NUM_13;
        gpio_num_t cs_io_num = GPIO_NUM_10;
        gpio_num_t reset_io_num = GPIO_NUM_9;
        gpio_num_t bl_io_num = GPIO_NUM_14;
        spi_host_device_t spi_host = SPI2_HOST;
        int width = 240;
        int height = 320;
        int pclk_hz = 20 * 1000 * 1000;
        int rotation = 0;
    };

    explicit LcdClock(const Config &config);
    ~LcdClock();

    bool begin();
    void update_time(int hour, int minute, int second);
    void set_brightness(bool on);

private:
    void draw_frame();
    void draw_digit(int x, int y, int value);
    void draw_two_digits(int x, int y, int value);
    void draw_colon(int x, int y);
    void draw_char(char ch, int x, int y);
    void draw_text(const std::string &text, int x, int y);
    void clear_screen(uint16_t color);
    void draw_rect(int x, int y, int w, int h, uint16_t color);
    void flush_screen();

    Config config_;
    bool initialized_ = false;
    int current_hour_ = 0;
    int current_minute_ = 0;
    int current_second_ = 0;
    std::vector<uint16_t> framebuffer_;
    esp_lcd_panel_io_handle_t io_handle_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
};

}  // namespace lcd_clock
