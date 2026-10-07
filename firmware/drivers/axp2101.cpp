#include "axp2101.hpp"

namespace hg {

std::optional<PowerStatus> Axp2101::read() {
  uint8_t status[2], enable = 0, adc = 0;
  if (!read_(0x00, status, 2) || !read_(0x18, &enable, 1) || !read_(0x30, &adc, 1)) return std::nullopt;
  PowerStatus out;
  out.battery_present = (status[0] & 0x08) != 0;
  out.external_power = (status[0] & 0x20) != 0;
  out.charging = *out.battery_present && (status[1] & 0x60) == 0x20;
  if (*out.battery_present) {
    if (adc & 0x01) {
      uint8_t voltage[2];
      if (!read_(0x34, voltage, 2)) return std::nullopt;
      const uint16_t mv = static_cast<uint16_t>(((voltage[0] & 0x3f) << 8) | voltage[1]);
      if (mv >= 2000 && mv <= 5000) out.battery_mv = mv;
    }
    if (enable & 0x08) {
      uint8_t percent;
      if (!read_(0xa4, &percent, 1)) return std::nullopt;
      if (percent <= 100) out.battery_percent = percent;
    }
  }
  return out;
}

bool Axp2101::power_off() {
  uint8_t config;
  return read_(0x10, &config, 1) && write_(0x10, static_cast<uint8_t>((config & ~0x02) | 0x01));
}

bool Axp2101::enable_aldo1_3v3() {
  uint8_t voltage, enabled;
  if (!read_(0x92, &voltage, 1) || !read_(0x90, &enabled, 1)) return false;
  // ALDO1: 500 mV + 100 mV per step. Preserve the other rail controls.
  return write_(0x92, static_cast<uint8_t>((voltage & 0xe0) | 28)) &&
         write_(0x90, static_cast<uint8_t>(enabled | 0x01));
}

bool Axp2101::enable_aldo2_3v3() {
  uint8_t voltage, enabled;
  if (!read_(0x93, &voltage, 1) || !read_(0x90, &enabled, 1)) return false;
  return write_(0x93, static_cast<uint8_t>((voltage & 0xe0) | 28)) &&
         write_(0x90, static_cast<uint8_t>(enabled | 0x02));
}

bool Axp2101::set_aldo3_3v3(bool on) {
  uint8_t voltage, enabled;
  if (!read_(0x94, &voltage, 1) || !read_(0x90, &enabled, 1)) return false;
  return write_(0x94, static_cast<uint8_t>((voltage & 0xe0) | 28)) &&
         write_(0x90, static_cast<uint8_t>(on ? enabled | 0x04 : enabled & ~0x04));
}

namespace {
constexpr uint8_t kPowerOffEnable = 0x22, kKeyLevels = 0x27, kIrqEnable2 = 0x41, kIrqStatus2 = 0x49;
constexpr uint8_t kKeyShortBit = 1 << 3, kKeyLongBit = 1 << 2;
}  // namespace

bool Axp2101::configure_power_key() {
  uint8_t off, levels, enable;
  if (!read_(kPowerOffEnable, &off, 1) || !read_(kKeyLevels, &levels, 1) || !read_(kIrqEnable2, &enable, 1)) return false;
  // 0x22 bit 1: a long press powers off; bit 0 clear: off rather than restart.
  // 0x27 bits 5:4 = 01: long-press event at 1.5 s; bits 3:2 = 01: power off at 6 s.
  return write_(kPowerOffEnable, static_cast<uint8_t>((off | 0x02) & ~0x01)) &&
         write_(kKeyLevels, static_cast<uint8_t>((levels & ~0x3c) | 0x10 | 0x04)) &&
         write_(kIrqEnable2, static_cast<uint8_t>(enable | kKeyShortBit | kKeyLongBit)) &&
         write_(kIrqStatus2, kKeyShortBit | kKeyLongBit);
}

unsigned Axp2101::take_power_key() {
  uint8_t status;
  if (!read_(kIrqStatus2, &status, 1)) return kKeyNone;
  status &= kKeyShortBit | kKeyLongBit;
  if (!status || !write_(kIrqStatus2, status)) return kKeyNone;  // write-1-to-clear
  return (status & kKeyShortBit ? unsigned(kKeyShort) : 0u) | (status & kKeyLongBit ? unsigned(kKeyLong) : 0u);
}

}  // namespace hg
