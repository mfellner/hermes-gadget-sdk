#pragma once

#include <functional>
#include <utility>

#include "hg/hal.hpp"

namespace hg {

// Register definitions follow X-Powers AXP2101 SWcharge v1.0, section 6.13.
// The port owns I2C. Reading never changes charging, rails, or gauge parameters.
class Axp2101 final : public Power {
 public:
  using Read = std::function<bool(uint8_t, uint8_t*, size_t)>;
  using Write = std::function<bool(uint8_t, uint8_t)>;
  Axp2101(Read read, Write write) : read_(std::move(read)), write_(std::move(write)) {}
  std::optional<PowerStatus> read() override;
  bool power_off() override;
  bool enable_aldo1_3v3();  // Only for boards whose audio circuit requires this rail.
  bool enable_aldo2_3v3();  // Boards whose speaker amplifier is enabled by this rail.
  // Boards whose panel reset is the ALDO3 rail: 3.3 V, then on (true) or off.
  bool set_aldo3_3v3(bool on);

  // Power key (PWRON). The PMIC keeps these settings across ESP32 resets, so
  // every app on a board writes the same values: long press powers off after
  // 6 s (not a restart), the long-press event comes at 1.5 s, short and long
  // press events enabled, stale events cleared. Other bits are preserved.
  bool configure_power_key();
  enum PowerKey : unsigned { kKeyNone = 0, kKeyShort = 1, kKeyLong = 2 };
  // Key events since the last call; clears them. kKeyNone when the read fails.
  unsigned take_power_key();

 private:
  Read read_;
  Write write_;
};

}  // namespace hg
