// Integration with the esp32-playground multi-app platform
// (https://github.com/mfellner/esp32-playground): this firmware is one app in
// an OTA slot next to a launcher. The board's keys reach the launcher (see
// PlatformKeyConfig), and the platform bootloader's crash guard is told once
// this app runs properly.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "driver/gpio.h"
#include "esp_log.h"

#if CONFIG_HG_PLATFORM_APP_SWITCH
#include "app_switch.h"
#endif

namespace hgp::platform {
namespace {

const char* TAG = "hg.platform";
// Startup crashes (drivers, Wi-Fi bring-up) happen well within this.
constexpr uint32_t kHealthyAfterMs = 15000;
constexpr uint32_t kTalkAfterMs = 300;      // KEY held this long talks
constexpr uint32_t kLauncherAfterMs = 1000;  // BOOT held this long opens the launcher
bool marked_healthy = false;

struct Key {
  int gpio = -1;
  bool down = false;      // debounced level
  uint8_t stable = 0;     // samples the raw level has disagreed with `down`
  bool armed = false;     // released once since boot (a key held through a restart is ignored)
  bool consumed = false;  // this press woke the screen, or already acted
  bool talking = false;
  uint32_t since = 0;
};
Key talk_key, cancel_key;

// Debounced press/release edges. Returns +1 pressed, -1 released, 0 nothing.
int sample(Key& k, uint32_t now) {
  if (k.gpio < 0) return 0;
  const bool raw = gpio_get_level(static_cast<gpio_num_t>(k.gpio)) == 0;
  if (!k.armed) {
    k.armed = !raw;
    return 0;
  }
  if (raw == k.down) {
    k.stable = 0;
    return 0;
  }
  if (++k.stable < 2) return 0;  // 20 ms at the app loop's pace
  k.stable = 0;
  k.down = raw;
  if (raw) k.since = now;
  return raw ? 1 : -1;
}

// Screens where a quick KEY press is an answer, not a way out.
bool wants_talk_tap(const hg::App& app) {
  return app.screen() == hg::Screen::Prompt || app.wifi_setup_open() || app.settings_open() || app.menu_open();
}

}  // namespace

void begin(const PlatformKeyConfig& keys) {
#if CONFIG_HG_PLATFORM_APP_SWITCH
  const app_switch_launch_reason_t reason = app_switch_take_launch_reason();
  ESP_LOGI(TAG, "started by the platform (reason %d)", static_cast<int>(reason.kind));
#endif
  talk_key.gpio = keys.talk;
  cancel_key.gpio = keys.cancel;
  uint64_t mask = 0;
  for (int gpio : {keys.talk, keys.cancel})
    if (gpio >= 0) mask |= 1ULL << gpio;
  if (mask) {
    gpio_config_t io = {};
    io.pin_bit_mask = mask;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
  }
}

void poll_keys(hg::App& app, uint32_t now) {
  switch (sample(talk_key, now)) {
    case 1:
      talk_key.consumed = app.display_sleeping() && app.wake_display();
      break;
    case -1:
      if (talk_key.talking) {
        app.on_button(hg::Button::Talk, false);
      } else if (!talk_key.consumed && wants_talk_tap(app)) {
        // A quick press answers where the screen asks for TALK; elsewhere it does nothing.
        app.on_button(hg::Button::Talk, true);
        app.on_button(hg::Button::Talk, false);
      }
      talk_key.talking = talk_key.consumed = false;
      break;
    default:
      if (talk_key.down && !talk_key.talking && !talk_key.consumed && now - talk_key.since >= kTalkAfterMs) {
        talk_key.talking = true;
        app.on_button(hg::Button::Talk, true);
      }
      break;
  }
  switch (sample(cancel_key, now)) {
    case 1:
      cancel_key.consumed = app.display_sleeping() && app.wake_display();
      break;
    case -1:
      if (!cancel_key.consumed) {
        app.on_button(hg::Button::Cancel, true);
        app.on_button(hg::Button::Cancel, false);
      }
      cancel_key.consumed = false;
      break;
    default:
      if (cancel_key.down && !cancel_key.consumed && now - cancel_key.since >= kLauncherAfterMs) {
        cancel_key.consumed = true;
        if (!open_launcher()) ESP_LOGW(TAG, "launcher unavailable");
      }
      break;
  }
}

bool open_launcher() {
#if CONFIG_HG_PLATFORM_APP_SWITCH
  return app_switch_open_launcher() == ESP_OK;  // restarts on success
#else
  return false;
#endif
}

void tick(uint32_t now_ms) {
#if CONFIG_HG_PLATFORM_APP_SWITCH
  if (!marked_healthy && now_ms >= kHealthyAfterMs) {
    marked_healthy = true;
    app_switch_mark_healthy();
    ESP_LOGI(TAG, "running; crash guard cleared");
  }
#else
  (void)now_ms;
#endif
}

}  // namespace hgp::platform
