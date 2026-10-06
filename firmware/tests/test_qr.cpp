// QR encoder checks against an independent implementation: the module
// patterns below are FNV-1a hashes of matrices produced by Project Nayuki's
// qrcodegen 1.8 (QrCode.encode_segments([QrSegment.make_bytes(text)],
// Ecc.MEDIUM, mask=N, boostecl=False)), rows of '#'/'.' joined by newlines,
// without the quiet zone. Each was also decoded by zxing-cpp.
#include <string>

#include "check.hpp"
#include "hg/qr.hpp"

namespace {

const std::string kTexts[] = {"http://192.168.4.1", "WIFI:T:WPA;S:Hermes-0428;P:3f9a1c2e;;", std::string(150, 'A'),
                              std::string(200, 'b')};

struct Expected {
  int text, mask, size;
  uint32_t hash;
};

const Expected kExpected[] = {
    {0, 0, 25, 0x5dee8c4bu},
    {0, 1, 25, 0x3533e74du},
    {0, 2, 25, 0x30d93c2du},
    {0, 3, 25, 0x00bd43e1u},
    {0, 4, 25, 0x7a569b73u},
    {0, 5, 25, 0xee5212fcu},
    {0, 6, 25, 0x0defd6cdu},
    {0, 7, 25, 0x216a6293u},
    {1, 0, 29, 0x8070741du},
    {1, 1, 29, 0xfb48172fu},
    {1, 2, 29, 0x6bc79762u},
    {1, 3, 29, 0x32d1c655u},
    {1, 4, 29, 0x69510440u},
    {1, 5, 29, 0x0e030cfau},
    {1, 6, 29, 0x34ee754bu},
    {1, 7, 29, 0x095e7d45u},
    {2, 0, 49, 0x39e2d3d3u},
    {2, 1, 49, 0x4216e898u},
    {2, 2, 49, 0xb05fd34fu},
    {2, 3, 49, 0x97504f6bu},
    {2, 4, 49, 0x7ff31b49u},
    {2, 5, 49, 0x67854ec6u},
    {2, 6, 49, 0x057beca7u},
    {2, 7, 49, 0xa8b8fafbu},
    {3, 0, 57, 0xd20d121fu},
    {3, 1, 57, 0xb9f3ce7cu},
    {3, 2, 57, 0xc577a57bu},
    {3, 3, 57, 0x2bb9b2dbu},
    {3, 4, 57, 0x25209e59u},
    {3, 5, 57, 0x18e3502du},
    {3, 6, 57, 0x544e9e5bu},
    {3, 7, 57, 0x7bc84e9fu},
};

uint32_t hash(const hg::qr::Code& c) {
  uint32_t h = 2166136261u;
  for (int y = 0; y < c.size; ++y) {
    if (y) h = (h ^ '\n') * 16777619u;
    for (int x = 0; x < c.size; ++x) h = (h ^ static_cast<uint8_t>(c.at(x, y) ? '#' : '.')) * 16777619u;
  }
  return h;
}

}  // namespace

TEST("qr: every mask matches an independent encoder, versions 2 to 10") {
  for (const Expected& e : kExpected) {
    const hg::qr::Code c = hg::qr::encode(kTexts[e.text], e.mask);
    CHECK_EQ(c.size, e.size);
    CHECK_EQ(hash(c), e.hash);
  }
}

TEST("qr: the automatic mask is one of the encodings above, and long text does not fit") {
  // qrcodegen picks masks 6, 2, 3 and 2 for these texts.
  const int reference_masks[] = {6, 2, 3, 2};
  for (int t = 0; t < 4; ++t) {
    const hg::qr::Code automatic = hg::qr::encode(kTexts[t]);
    const hg::qr::Code forced = hg::qr::encode(kTexts[t], reference_masks[t]);
    CHECK(automatic.dark == forced.dark);
  }
  CHECK_EQ(hg::qr::encode(std::string(214, 'x')).size, 0);  // beyond version 10-M
  CHECK_EQ(hg::qr::encode(std::string(213, 'x')).size, 57);
}

TEST("qr: Wi-Fi payloads escape the reserved characters") {
  CHECK_EQ(hg::qr::wifi_payload("Hermes-0428", "3f9a1c2e"), std::string("WIFI:T:WPA;S:Hermes-0428;P:3f9a1c2e;;"));
  CHECK_EQ(hg::qr::wifi_payload("a;b,c", R"(p:q\"r)"), std::string(R"(WIFI:T:WPA;S:a\;b\,c;P:p\:q\\\"r;;)"));
}
