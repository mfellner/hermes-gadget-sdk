// QSPI AMOLED panels driven directly through esp_lcd panel IO in quad mode:
// CO5300 (the round 466x466 1.75" modules) and the SH8601-family controller of
// the 480x480 2.16" module.
//
// Framing on the QSPI link: commands go out as (0x02 << 24) | (cmd << 8) with
// their parameters; pixels as (0x32 << 24) | (RAMWR << 8). The controller only
// accepts windows that start on an even row/column and span an even count.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>
#include <iterator>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.amoled";
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr int kBounceRows = 16;  // even, so every chunk keeps the window even

constexpr uint32_t command_word(uint8_t cmd) { return (0x02u << 24) | (static_cast<uint32_t>(cmd) << 8); }
constexpr uint32_t kRamWrite = (0x32u << 24) | (0x2Cu << 8);

struct InitCommand {
  uint8_t cmd;
  uint8_t data[4];
  uint8_t len;
  uint16_t delay_ms;
};

// Panel bring-up for CO5300 1.75" modules: vendor page settings, RGB565,
// tearing line on, full brightness, the 466x466 window (column offset 6),
// then sleep out and display on.
constexpr InitCommand kInit[] = {
    {0x36, {0x00}, 1, 0},  // memory access control: no rotation
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0xFE, {0x20}, 1, 0},
    {0x19, {0x10}, 1, 0},
    {0x1C, {0xA0}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, {}, 0, 600},  // sleep out
    {0x29, {}, 0, 0},    // display on
};

// The 2.16" 480x480 module (Waveshare ESP32-C6-Touch-AMOLED-2.16), as the vendor's
// LVGL example brings it up through esp_lcd_sh8601 (Waveshare repository
// 294543798f1a44e2f2c4d2976522323f2beee11d, 02_Example/ESP-IDF-v5.5.3/09_LVGL_V9_Test):
// that driver first sends MADCTL 0x00 and COLMOD 0x55, then the vendor table.
// The panel was reset through its power rail just before.
constexpr InitCommand kInitSh8601[] = {
    {0x36, {0x00}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x11, {}, 0, 600},  // sleep out
    {0xFE, {0x20}, 1, 0},
    {0x19, {0x10}, 1, 0},
    {0x1C, {0xA0}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x35, {0x00}, 1, 0},
    {0x36, {0x30}, 1, 0},
    {0x53, {0x20}, 1, 0},
    {0x51, {0xFF}, 1, 0},
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, {}, 0, 100},  // display on
};

}  // namespace

bool AmoledDisplay::on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<AmoledDisplay*>(ctx)->done_, &woken);
  return woken == pdTRUE;
}

void AmoledDisplay::command(uint8_t cmd, const uint8_t* data, size_t len) {
  esp_lcd_panel_io_tx_param(io_, static_cast<int>(command_word(cmd)), len ? data : nullptr, len);
}

bool AmoledDisplay::begin(const AmoledConfig& cfg, const std::function<void()>& panel_reset) {
  cfg_ = cfg;
  const size_t px = static_cast<size_t>(cfg.width) * cfg.height;
  size_t chunk_rows = kBounceRows;
  if (cfg.strip_rows) {
    // No framebuffer: two internal DMA strips, allocated before Wi-Fi fragments the heap.
    chunk_rows = cfg.strip_rows;
    for (auto& s : strips_) {
      s = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(cfg.width) * cfg.strip_rows * 2,
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (!strips_[0] || !strips_[1]) {
      ESP_LOGE(TAG, "not enough memory for two %ux%u strips", cfg.width, cfg.strip_rows);
      return false;
    }
  } else {
    fb_ = static_cast<uint16_t*>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bounce_ = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(cfg.width) * kBounceRows * 2, MALLOC_CAP_DMA));
    if (!fb_ || !bounce_) {
      ESP_LOGE(TAG, "not enough memory for a %ux%u framebuffer (is PSRAM enabled?)", cfg.width, cfg.height);
      return false;
    }
    std::memset(fb_, 0, px * 2);
  }
  done_ = xSemaphoreCreateBinary();

  spi_bus_config_t bus = {};
  bus.sclk_io_num = cfg.sclk;
  bus.data0_io_num = cfg.d0;
  bus.data1_io_num = cfg.d1;
  bus.data2_io_num = cfg.d2;
  bus.data3_io_num = cfg.d3;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = static_cast<int>(cfg.width * chunk_rows * 2 + 16);
  bus.flags = SPICOMMON_BUSFLAG_QUAD;
  ESP_ERROR_CHECK(spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(cfg.cs);
  io_cfg.dc_gpio_num = GPIO_NUM_NC;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = static_cast<uint32_t>(cfg.qspi_mhz) * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 32;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = 1;
  io_cfg.on_color_trans_done = &AmoledDisplay::on_trans_done;
  io_cfg.user_ctx = this;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kHost), &io_cfg, &io_));

  if (cfg.rst >= 0) {
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << cfg.rst;
    rst.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 1);
    vTaskDelay(pdMS_TO_TICKS(150));
  } else if (panel_reset) {
    panel_reset();
  }
  const bool sh8601 = cfg.init == AmoledInit::Sh8601_480;
  const InitCommand* table = sh8601 ? kInitSh8601 : kInit;
  const size_t count = sh8601 ? std::size(kInitSh8601) : std::size(kInit);
  for (size_t i = 0; i < count; ++i) {
    command(table[i].cmd, table[i].data, table[i].len);
    if (table[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(table[i].delay_ms));
  }
  ESP_LOGI(TAG, "%s %ux%u ready%s", sh8601 ? "SH8601" : "CO5300", cfg.width, cfg.height,
           cfg.strip_rows ? " (strips)" : "");
  return true;
}

hg::DisplayInfo AmoledDisplay::info() const {
  hg::DisplayInfo di;
  di.width = cfg_.width;
  di.height = cfg_.height;
  di.swap_bytes = true;  // big-endian RGB565 on the wire
  di.has_backlight = true;  // brightness command 0x51
  di.round = cfg_.round;
  di.strip_rows = cfg_.strip_rows;
  di.inset = cfg_.inset;
  // Windows start on an even row and span an even count; flush() widens its
  // rows itself, strips are cut on even rows by the UI.
  if (cfg_.strip_rows) di.row_align = 2;
  return di;
}

void AmoledDisplay::window(int y0, int y1) {
  const int x0 = cfg_.gap_x, x1 = cfg_.gap_x + cfg_.width - 1;
  const int ya = y0 + cfg_.gap_y, yb = y1 - 1 + cfg_.gap_y;
  const uint8_t cols[4] = {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1 >> 8),
                           static_cast<uint8_t>(x1)};
  const uint8_t lines[4] = {static_cast<uint8_t>(ya >> 8), static_cast<uint8_t>(ya), static_cast<uint8_t>(yb >> 8),
                            static_cast<uint8_t>(yb)};
  // Parameter writes wait for every queued pixel transfer first (esp_lcd SPI IO).
  command(0x2A, cols, 4);
  command(0x2B, lines, 4);
}

uint16_t* AmoledDisplay::strip(uint16_t) {
  // The other strip's transfer was drained by the window commands of the last
  // present(), so it is free; the current one may still be on the wire.
  return strips_[next_strip_];
}

void AmoledDisplay::present(uint16_t y0, uint16_t y1) {
  if (!strips_[0] || y1 <= y0) return;
  window(y0, y1);
  esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), strips_[next_strip_],
                            static_cast<size_t>(y1 - y0) * cfg_.width * 2);
  next_strip_ ^= 1;
}

void AmoledDisplay::flush(uint16_t y0, uint16_t y1) {
  const int w = cfg_.width, h = cfg_.height;
  // Even start and even span: widen the dirty rows by at most one on each side.
  int top = y0 & ~1;
  int bottom = std::min(h, (y1 + 1) & ~1);
  for (int y = top; y < bottom; y += kBounceRows) {
    const int rows = std::min(kBounceRows, bottom - y);
    std::memcpy(bounce_, fb_ + static_cast<size_t>(y) * w, static_cast<size_t>(rows) * w * 2);
    window(y, y + rows);
    esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), bounce_, static_cast<size_t>(rows) * w * 2);
    // The bounce buffer is reused: wait until the DMA transfer has finished.
    xSemaphoreTake(done_, pdMS_TO_TICKS(100));
  }
}

void AmoledDisplay::set_backlight(uint8_t percent) {
  const uint8_t level = static_cast<uint8_t>(255u * std::min<uint8_t>(percent, 100) / 100u);
  command(0x51, &level, 1);
}

}  // namespace hgp
