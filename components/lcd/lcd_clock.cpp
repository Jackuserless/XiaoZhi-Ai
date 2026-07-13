#include "lcd_clock.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

namespace lcd_clock {
namespace {
static constexpr char *TAG = "lcd_clock";

constexpr uint16_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t red = (r >> 3) & 0x1F;
    uint16_t green = (g >> 2) & 0x3F;
    uint16_t blue = (b >> 3) & 0x1F;
    return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

constexpr uint16_t COLOR_BLACK = make_color(0, 0, 0);
constexpr uint16_t COLOR_WHITE = make_color(255, 255, 255);
constexpr uint16_t COLOR_BLUE = make_color(0, 0, 255);
constexpr uint16_t COLOR_RED = make_color(255, 0, 0);
constexpr uint16_t COLOR_BG = make_color(15, 20, 30);

void write_pixel(std::vector<uint16_t> &framebuffer, int x, int y, int width, uint16_t color) {
    if (x < 0 || y < 0 || x >= width || y >= 320) {
        return;
    }
    framebuffer.at(static_cast<size_t>(y) * width + x) = color;
}

}  // namespace

LcdClock::LcdClock(const Config &config) : config_(config) {}

LcdClock::~LcdClock() {
    if (!initialized_) {
        return;
    }
    if (panel_handle_ != nullptr) {
        esp_lcd_panel_disp_on_off(panel_handle_, false);
        esp_lcd_panel_del(panel_handle_);
    }
    if (io_handle_ != nullptr) {
        esp_lcd_panel_io_del(io_handle_);
    }
    spi_bus_free(config_.spi_host);
    gpio_reset_pin(config_.bl_io_num);
    gpio_reset_pin(config_.reset_io_num);
    gpio_reset_pin(config_.dc_io_num);
    gpio_reset_pin(config_.cs_io_num);
}

bool LcdClock::begin() {
    if (initialized_) {
        return true;
    }

    framebuffer_.assign(static_cast<size_t>(config_.width) * config_.height, COLOR_BLACK);

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << config_.bl_io_num) | (1ULL << config_.reset_io_num) |
                           (1ULL << config_.dc_io_num) | (1ULL << config_.cs_io_num);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    gpio_set_level(config_.bl_io_num, 0);
    gpio_set_level(config_.reset_io_num, 1);
    gpio_set_level(config_.dc_io_num, 0);
    gpio_set_level(config_.cs_io_num, 1);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = config_.sclk_io_num;
    buscfg.mosi_io_num = config_.mosi_io_num;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;

    err = spi_bus_initialize(config_.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = config_.dc_io_num;
    io_config.cs_gpio_num = config_.cs_io_num;
    io_config.pclk_hz = config_.pclk_hz;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)config_.spi_host, &io_config, &io_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s", esp_err_to_name(err));
        spi_bus_free(config_.spi_host);
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = config_.reset_io_num;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;

    err = esp_lcd_new_panel_st7789(io_handle_, &panel_config, &panel_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(io_handle_);
        io_handle_ = nullptr;
        spi_bus_free(config_.spi_host);
        return false;
    }

    esp_lcd_panel_reset(panel_handle_);
    esp_lcd_panel_init(panel_handle_);
    esp_lcd_panel_invert_color(panel_handle_, false);
    esp_lcd_panel_disp_on_off(panel_handle_, true);
    set_brightness(true);

    initialized_ = true;
    clear_screen(COLOR_BG);
    flush_screen();
    ESP_LOGI(TAG, "LCD clock initialized");
    return true;
}

void LcdClock::update_time(int hour, int minute, int second) {
    if (!initialized_) {
        return;
    }
    current_hour_ = hour;
    current_minute_ = minute;
    current_second_ = second;
    draw_frame();
}

void LcdClock::set_brightness(bool on) {
    if (initialized_) {
        gpio_set_level(config_.bl_io_num, on ? 1 : 0);
    }
}

void LcdClock::draw_frame() {
    clear_screen(COLOR_BG);
    draw_text("TIME", 18, 18);
    draw_two_digits(36, 80, current_hour_);
    draw_colon(110, 100);
    draw_two_digits(128, 80, current_minute_);
    draw_colon(202, 100);
    draw_two_digits(220, 80, current_second_);
    flush_screen();
}

void LcdClock::draw_digit(int x, int y, int value) {
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
    const int top = y;
    const int mid = y + 28;
    const int bottom = y + 56;
    const int left = x;
    const int right = x + seg_w + gap;

    if (segments[value][0]) {
        draw_rect(x + 2, y, seg_w, seg_h, COLOR_WHITE);
    }
    if (segments[value][1]) {
        draw_rect(x, y + 8, seg_h, seg_w, COLOR_WHITE);
    }
    if (segments[value][2]) {
        draw_rect(x + seg_h + gap, y + 8, seg_h, seg_w, COLOR_WHITE);
    }
    if (segments[value][3]) {
        draw_rect(x + 2, y + 28, seg_w, seg_h, COLOR_WHITE);
    }
    if (segments[value][4]) {
        draw_rect(x, y + 36, seg_h, seg_w, COLOR_WHITE);
    }
    if (segments[value][5]) {
        draw_rect(x + seg_h + gap, y + 36, seg_h, seg_w, COLOR_WHITE);
    }
    if (segments[value][6]) {
        draw_rect(x + 2, y + 56, seg_w, seg_h, COLOR_WHITE);
    }
}

void LcdClock::draw_two_digits(int x, int y, int value) {
    if (value < 0) {
        value = 0;
    }
    draw_digit(x, y, (value / 10) % 10);
    draw_digit(x + 28, y, value % 10);
}

void LcdClock::draw_colon(int x, int y) {
    draw_rect(x, y + 10, 4, 4, COLOR_RED);
    draw_rect(x, y + 28, 4, 4, COLOR_RED);
}

void LcdClock::draw_char(char ch, int x, int y) {
    (void)ch;
    (void)x;
    (void)y;
}

void LcdClock::draw_text(const std::string &text, int x, int y) {
    int cursor_x = x;
    for (char ch : text) {
        draw_char(ch, cursor_x, y);
        cursor_x += 12;
    }
}

void LcdClock::clear_screen(uint16_t color) {
    std::fill(framebuffer_.begin(), framebuffer_.end(), color);
}

void LcdClock::draw_rect(int x, int y, int w, int h, uint16_t color) {
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            write_pixel(framebuffer_, xx, yy, config_.width, color);
        }
    }
}

void LcdClock::flush_screen() {
    if (panel_handle_ != nullptr) {
        esp_lcd_panel_draw_bitmap(panel_handle_, 0, 0, config_.width - 1, config_.height - 1, framebuffer_.data());
    }
}

}  // namespace lcd_clock
