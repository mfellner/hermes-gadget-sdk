// QR codes (ISO/IEC 18004) for on-screen setup links: byte mode, error
// correction level M, versions 1-10 (up to 213 bytes). Deterministic, no
// allocation beyond the returned matrix.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hg::qr {

struct Code {
  int size = 0;                // modules per side (0: the text did not fit)
  std::vector<uint8_t> dark;   // size * size, row-major, 1 = dark module
  bool at(int x, int y) const { return dark[static_cast<size_t>(y * size + x)] != 0; }
};

// Encodes `text` with the lowest version that holds it. mask < 0 picks the
// mask with the lowest penalty, as the standard asks; 0-7 forces one (tests).
Code encode(std::string_view text, int mask = -1);

// The payload a phone camera turns into "join this Wi-Fi network" (WPA/WPA2),
// with the special characters escaped.
std::string wifi_payload(std::string_view ssid, std::string_view password);

}  // namespace hg::qr
