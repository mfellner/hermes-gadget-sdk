// QR code encoder, written from ISO/IEC 18004:2015: byte mode, error
// correction level M, versions 1-10.
#include "hg/qr.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace hg::qr {
namespace {

constexpr int kMaxVersion = 10;
// Level M per version (index 0 unused): error correction codewords per block,
// and the number of blocks (ISO/IEC 18004 table 9).
constexpr int kEccPerBlock[kMaxVersion + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int kBlocks[kMaxVersion + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
constexpr int kFormatLevelM = 0;  // format information bits for level M

// Modules available for data and error correction in a version.
int raw_modules(int version) {
  int n = (16 * version + 128) * version + 64;
  if (version >= 2) {
    const int align = version / 7 + 2;
    n -= (25 * align - 10) * align - 55;
    if (version >= 7) n -= 36;
  }
  return n;
}

int data_codewords(int version) { return raw_modules(version) / 8 - kEccPerBlock[version] * kBlocks[version]; }

uint8_t gf_mul(uint8_t x, uint8_t y) {
  int z = 0;
  for (int i = 7; i >= 0; --i) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return static_cast<uint8_t>(z);
}

std::vector<uint8_t> rs_divisor(int degree) {
  std::vector<uint8_t> d(static_cast<size_t>(degree), 0);
  d.back() = 1;
  uint8_t root = 1;
  for (int i = 0; i < degree; ++i) {
    for (size_t j = 0; j < d.size(); ++j) {
      d[j] = gf_mul(d[j], root);
      if (j + 1 < d.size()) d[j] ^= d[j + 1];
    }
    root = gf_mul(root, 0x02);
  }
  return d;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor) {
  std::vector<uint8_t> r(divisor.size(), 0);
  for (uint8_t b : data) {
    const uint8_t factor = static_cast<uint8_t>(b ^ r[0]);
    r.erase(r.begin());
    r.push_back(0);
    for (size_t i = 0; i < r.size(); ++i) r[i] ^= gf_mul(divisor[i], factor);
  }
  return r;
}

std::vector<int> alignment_positions(int version, int size) {
  if (version == 1) return {};
  const int count = version / 7 + 2;
  const int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
  std::vector<int> pos;
  for (int i = 0, p = size - 7; i < count - 1; ++i, p -= step) pos.insert(pos.begin(), p);
  pos.insert(pos.begin(), 6);
  return pos;
}

bool masked(int mask, int x, int y) {
  switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
  }
}

class Matrix {
 public:
  explicit Matrix(int version) : v_(version), n_(version * 4 + 17), dark_(size2(), 0), fn_(size2(), 0) {}

  int size() const { return n_; }
  bool dark(int x, int y) const { return dark_[idx(x, y)] != 0; }

  void function_patterns() {
    for (int i = 0; i < n_; ++i) {
      set_fn(6, i, i % 2 == 0);
      set_fn(i, 6, i % 2 == 0);
    }
    finder(3, 3);
    finder(n_ - 4, 3);
    finder(3, n_ - 4);
    const std::vector<int> a = alignment_positions(v_, n_);
    const size_t last = a.empty() ? 0 : a.size() - 1;
    for (size_t i = 0; i < a.size(); ++i)
      for (size_t j = 0; j < a.size(); ++j)
        if (!((i == 0 && j == 0) || (i == 0 && j == last) || (i == last && j == 0))) alignment(a[i], a[j]);
    format(0);  // reserve; the real bits come after masking
    version_info();
  }

  void codewords(const std::vector<uint8_t>& data) {
    size_t bit = 0;
    for (int right = n_ - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5;
      for (int vert = 0; vert < n_; ++vert) {
        for (int j = 0; j < 2; ++j) {
          const int x = right - j;
          const bool upward = ((right + 1) & 2) == 0;
          const int y = upward ? n_ - 1 - vert : vert;
          if (fn_[idx(x, y)] || bit >= data.size() * 8) continue;
          dark_[idx(x, y)] = (data[bit >> 3] >> (7 - (bit & 7))) & 1;
          ++bit;
        }
      }
    }
  }

  void apply_mask(int mask) {
    for (int y = 0; y < n_; ++y)
      for (int x = 0; x < n_; ++x)
        if (!fn_[idx(x, y)] && masked(mask, x, y)) dark_[idx(x, y)] ^= 1;
  }

  void format(int mask) {
    const int data = kFormatLevelM << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) set_fn(8, i, bit(i));
    set_fn(8, 7, bit(6));
    set_fn(8, 8, bit(7));
    set_fn(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) set_fn(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) set_fn(n_ - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) set_fn(8, n_ - 15 + i, bit(i));
    set_fn(8, n_ - 8, true);  // the dark module
  }

  // Penalty score of the current pattern (ISO/IEC 18004 7.8.3).
  long penalty() const {
    long score = 0;
    for (int pass = 0; pass < 2; ++pass) {  // rows, then columns
      for (int a = 0; a < n_; ++a) {
        int run = 1;
        for (int b = 1; b <= n_; ++b) {
          if (b < n_ && at(pass, a, b) == at(pass, a, b - 1)) {
            ++run;
            continue;
          }
          if (run >= 5) score += 3 + (run - 5);
          run = 1;
        }
        // 1:1:3:1:1 finder-like pattern with four light modules on one side.
        static const uint8_t kPattern[7] = {1, 0, 1, 1, 1, 0, 1};
        for (int b = 0; b + 7 <= n_; ++b) {
          bool match = true;
          for (int k = 0; k < 7 && match; ++k) match = at(pass, a, b + k) == kPattern[k];
          if (!match) continue;
          auto light = [&](int from, int to) {
            for (int k = from; k < to; ++k)
              if (k >= 0 && k < n_ && at(pass, a, k)) return false;
            return true;
          };
          // Four light modules on one side and at least one on the other; outside
          // the symbol counts as light. Each side scores on its own.
          if (light(b - 4, b) && light(b + 7, b + 8)) score += 40;
          if (light(b + 7, b + 11) && light(b - 1, b)) score += 40;
        }
      }
    }
    for (int y = 0; y + 1 < n_; ++y)
      for (int x = 0; x + 1 < n_; ++x) {
        const uint8_t c = dark_[idx(x, y)];
        if (c == dark_[idx(x + 1, y)] && c == dark_[idx(x, y + 1)] && c == dark_[idx(x + 1, y + 1)]) score += 3;
      }
    long darks = 0;
    for (uint8_t d : dark_) darks += d;
    const long total = static_cast<long>(n_) * n_;
    score += ((std::labs(darks * 20 - total * 10) + total - 1) / total - 1) * 10;
    return score;
  }

  std::vector<uint8_t> modules() const { return dark_; }

 private:
  size_t size2() const { return static_cast<size_t>(n_) * static_cast<size_t>(n_); }
  size_t idx(int x, int y) const { return static_cast<size_t>(y * n_ + x); }
  uint8_t at(int pass, int a, int b) const { return pass == 0 ? dark_[idx(b, a)] : dark_[idx(a, b)]; }
  void set_fn(int x, int y, bool d) {
    dark_[idx(x, y)] = d;
    fn_[idx(x, y)] = 1;
  }
  void finder(int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
      for (int dx = -4; dx <= 4; ++dx) {
        const int x = cx + dx, y = cy + dy, dist = std::max(std::abs(dx), std::abs(dy));
        if (x >= 0 && x < n_ && y >= 0 && y < n_) set_fn(x, y, dist != 2 && dist != 4);
      }
  }
  void alignment(int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx) set_fn(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
  }
  void version_info() {
    if (v_ < 7) return;
    int rem = v_;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const long bits = static_cast<long>(v_) << 12 | rem;
    for (int i = 0; i < 18; ++i) {
      const bool d = ((bits >> i) & 1) != 0;
      const int a = n_ - 11 + i % 3, b = i / 3;
      set_fn(a, b, d);
      set_fn(b, a, d);
    }
  }

  int v_, n_;
  std::vector<uint8_t> dark_, fn_;
};

}  // namespace

Code encode(std::string_view text, int mask) {
  int version = 1;
  const int count_bits = 8;  // byte mode, versions 1-9 (version 10 uses 16)
  for (;; ++version) {
    if (version > kMaxVersion) return {};
    const int cc = version >= 10 ? 16 : count_bits;
    if (4 + cc + 8 * static_cast<long>(text.size()) <= data_codewords(version) * 8L) break;
  }
  // Data bits: mode, length, bytes, terminator, padding.
  std::vector<uint8_t> bits;
  auto put = [&](uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) bits.push_back((value >> i) & 1);
  };
  put(0x4, 4);
  put(static_cast<uint32_t>(text.size()), version >= 10 ? 16 : 8);
  for (char c : text) put(static_cast<uint8_t>(c), 8);
  const size_t capacity = static_cast<size_t>(data_codewords(version)) * 8;
  put(0, static_cast<int>(std::min<size_t>(4, capacity - bits.size())));
  while (bits.size() % 8) bits.push_back(0);
  std::vector<uint8_t> data;
  for (size_t i = 0; i < bits.size(); i += 8) {
    uint8_t b = 0;
    for (size_t j = 0; j < 8; ++j) b = static_cast<uint8_t>(b << 1 | bits[i + j]);
    data.push_back(b);
  }
  for (uint8_t pad = 0xEC; data.size() < static_cast<size_t>(data_codewords(version)); pad ^= 0xEC ^ 0x11) data.push_back(pad);

  // Error correction per block, then interleave.
  const int blocks = kBlocks[version], ecc_len = kEccPerBlock[version];
  const int raw = raw_modules(version) / 8;
  const int short_blocks = blocks - raw % blocks, short_len = raw / blocks;
  const std::vector<uint8_t> divisor = rs_divisor(ecc_len);
  std::vector<std::vector<uint8_t>> parts;
  for (int i = 0, k = 0; i < blocks; ++i) {
    const int len = short_len - ecc_len + (i < short_blocks ? 0 : 1);
    std::vector<uint8_t> block(data.begin() + k, data.begin() + k + len);
    k += len;
    const std::vector<uint8_t> ecc = rs_remainder(block, divisor);
    if (i < short_blocks) block.push_back(0);
    block.insert(block.end(), ecc.begin(), ecc.end());
    parts.push_back(std::move(block));
  }
  std::vector<uint8_t> codewords;
  for (size_t i = 0; i < parts[0].size(); ++i)
    for (size_t j = 0; j < parts.size(); ++j)
      if (i != static_cast<size_t>(short_len - ecc_len) || static_cast<int>(j) >= short_blocks)
        codewords.push_back(parts[j][i]);

  Matrix m(version);
  m.function_patterns();
  m.codewords(codewords);
  if (mask < 0 || mask > 7) {
    long best = -1;
    for (int candidate = 0; candidate < 8; ++candidate) {
      Matrix trial = m;
      trial.apply_mask(candidate);
      trial.format(candidate);
      const long p = trial.penalty();
      if (best < 0 || p < best) {
        best = p;
        mask = candidate;
      }
    }
  }
  m.apply_mask(mask);
  m.format(mask);
  return Code{m.size(), m.modules()};
}

std::string wifi_payload(std::string_view ssid, std::string_view password) {
  auto escape = [](std::string_view s) {
    std::string out;
    for (char c : s) {
      if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out.push_back('\\');
      out.push_back(c);
    }
    return out;
  };
  return "WIFI:T:WPA;S:" + escape(ssid) + ";P:" + escape(password) + ";;";
}

}  // namespace hg::qr
