// Pixel-exact rendering checks: a scripted session covering every screen, on
// several panel shapes. Each step's framebuffer hash is pinned, so a change
// to the renderer or the way it reaches the panel shows up here.
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/app.hpp"
#include "hg/protocol.hpp"

namespace {

struct Panel {
  int width, height;
  bool round;
  int row_align = 1;
  int strip_rows = 0;  // 0: framebuffer display
};

constexpr uint16_t kPoison = 0xF81F;  // what a strip holds before the UI draws it

struct RenderHal : hg::Display, hg::AudioIn, hg::AudioOut, hg::Transport, hg::Storage, hg::System {
  explicit RenderHal(Panel p) : panel(p), fb(static_cast<size_t>(p.width * p.height), 0) {
    if (p.strip_rows) strip_buf.assign(static_cast<size_t>(p.width * p.strip_rows), kPoison);
  }

  Panel panel;
  std::vector<uint16_t> fb;  // what the panel shows
  std::vector<uint16_t> strip_buf;
  int strip_y = -1;          // panel row of the strip handed out, -1 when none
  int strips = 0, bad_presents = 0;

  // System
  uint32_t clock = 1000;
  uint32_t now_ms() override { return clock; }
  void random_bytes(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  void log(hg::LogLevel, std::string_view) override {}

  // Transport
  void connect(const std::string&, const std::string&) override {}
  bool send_text(std::string_view) override { return true; }
  bool send_binary(const uint8_t*, size_t) override { return true; }
  void close() override {}

  // Storage
  std::map<std::string, std::string> kv;
  std::optional<std::string> get(std::string_view key) override {
    auto it = kv.find(std::string(key));
    if (it == kv.end()) return std::nullopt;
    return it->second;
  }
  void set(std::string_view key, std::string_view value) override { kv[std::string(key)] = std::string(value); }
  void erase(std::string_view key) override { kv.erase(std::string(key)); }

  // Display
  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = static_cast<uint16_t>(panel.width);
    d.height = static_cast<uint16_t>(panel.height);
    d.round = panel.round;
    d.swap_bytes = true;
    d.row_align = static_cast<uint8_t>(panel.row_align);
    d.strip_rows = static_cast<uint16_t>(panel.strip_rows);
    return d;
  }
  uint16_t* framebuffer() override { return panel.strip_rows ? nullptr : fb.data(); }
  void flush(uint16_t, uint16_t) override {
    if (panel.strip_rows) ++bad_presents;  // strip displays are never flushed
  }
  uint16_t* strip(uint16_t y0) override {
    if (!panel.strip_rows) return nullptr;
    std::fill(strip_buf.begin(), strip_buf.end(), kPoison);  // stale contents must never show
    strip_y = y0;
    ++strips;
    return strip_buf.data();
  }
  void present(uint16_t y0, uint16_t y1) override {
    const int a = panel.row_align;
    if (!panel.strip_rows || y0 != strip_y || y1 <= y0 || y1 - y0 > panel.strip_rows || y0 % a ||
        (y1 % a && y1 != panel.height) || y1 > panel.height) {
      ++bad_presents;
      return;
    }
    std::copy(strip_buf.begin(), strip_buf.begin() + (y1 - y0) * panel.width, fb.begin() + y0 * panel.width);
    strip_y = -1;
  }

  // Audio
  bool start(uint32_t) override { return true; }
  void stop() override {}
  bool begin(uint32_t) override { return spk = true; }
  void write(const int16_t*, size_t) override {}
  void end() override { spk = false; }
  void abort() override { spk = false; }
  bool busy() const override { return spk; }
  bool spk = false;

  hg::Hal hal() {
    hg::Hal h;
    h.system = this;
    h.transport = this;
    h.storage = this;
    h.display = this;
    h.mic = this;
    h.speaker = this;
    return h;
  }
};

uint32_t fnv(const std::vector<uint16_t>& px) {
  uint32_t h = 2166136261u;
  for (uint16_t v : px) {
    h = (h ^ (v & 0xFF)) * 16777619u;
    h = (h ^ (v >> 8)) * 16777619u;
  }
  return h;
}

hg::DeviceProfile touch_profile() {
  hg::DeviceProfile p;
  p.board = "render-board";
  p.firmware = "9.9.9";
  p.default_server_url = "ws://hermes.local:8765/gadget";
  p.touch_screen = true;
  p.cancel_label = "Swipe down";
  return p;
}

void image_frames(hg::App& app, int w, int h, uint8_t stream, size_t chunk) {
  std::vector<uint8_t> px(static_cast<size_t>(w * h * 2));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint16_t c = hg::rgb565(static_cast<uint8_t>(x * 255 / w), static_cast<uint8_t>(y * 255 / h),
                              static_cast<uint8_t>((x ^ y) & 0xFF));
      px[static_cast<size_t>((y * w + x) * 2)] = static_cast<uint8_t>(c & 0xFF);
      px[static_cast<size_t>((y * w + x) * 2 + 1)] = static_cast<uint8_t>(c >> 8);
    }
  uint16_t seq = 0;
  for (size_t off = 0; off < px.size(); off += chunk) {
    size_t n = std::min(chunk, px.size() - off);
    std::vector<uint8_t> frame(hg::proto::kBinaryHeader + n);
    hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Image, stream, seq++);
    std::memcpy(frame.data() + hg::proto::kBinaryHeader, px.data() + off, n);
    app.on_transport_binary(frame.data(), frame.size());
  }
}

// Runs the scripted session; `observe` sees the panel after every step.
void run_session(RenderHal& hal, size_t image_chunk, const std::function<void(const char*)>& observe) {
  hg::Hal h = hal.hal();
  hg::App app(h, touch_profile());
  auto advance = [&](uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      hal.clock += 10;
      app.tick();
    }
  };
  auto server = [&](const char* json) { app.on_transport_text(json); };

  app.begin();
  advance(300);
  observe("boot");
  app.on_network(true, "render-wifi");
  advance(1500);
  observe("connecting");
  app.on_transport_open();
  server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":false})");
  server(R"({"type":"pairing","code":"482913","command":"hermes pairing approve gadget 482913"})");
  advance(300);
  observe("pairing");
  server(R"({"type":"paired"})");
  advance(300);
  observe("ready");
  for (int i = 0; i < 12; ++i) {
    advance(500);
    observe("idle-anim");
  }
  app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> loud(320, 9000), quiet(320, 300);
  for (int i = 0; i < 6; ++i) {
    app.on_mic_samples(i % 2 ? loud.data() : quiet.data(), 320);
    advance(100);
    observe("listening");
  }
  app.on_button(hg::Button::Talk, false);
  server(R"({"type":"turn.start","turn":"t1"})");
  server(R"({"type":"transcript","turn":"t1","text":"what is on my calendar today"})");
  server(R"({"type":"status","turn":"t1","text":"Checking the calendar"})");
  for (int i = 0; i < 4; ++i) {
    advance(200);
    observe("thinking");
  }
  server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  for (int i = 0; i < 4; ++i) {
    advance(200);
    observe("speaking");
  }
  server(R"({"type":"reply.delta","turn":"t1","text":"You have three meetings"})");
  advance(200);
  observe("reply-delta");
  server(R"({"type":"reply","turn":"t1","text":"You have three meetings today: a design review at 10, lunch with Sam at 12:30 and the weekly sync at 16:00. Nothing is scheduled after that, so the evening is free. Want me to add a reminder for the sync?"})");
  server(R"({"type":"audio.end","stream":2})");
  advance(300);
  observe("reply-long");
  app.on_button(hg::Button::Down, true);
  app.on_button(hg::Button::Down, false);
  advance(200);
  observe("reply-scrolled");
  server(R"({"type":"turn.end","turn":"t1","outcome":"success"})");
  server(R"({"type":"display","title":"Weather","body":"Sunny, 21 C\nWind 3 m/s","ttl_s":30})");
  advance(300);
  observe("card");
  server(R"({"type":"prompt","id":"q1","title":"Allow command?","text":"rm -rf build"})");
  advance(300);
  observe("prompt");
  app.console("no");
  advance(300);
  observe("prompt-answered");
  {
    const int w = 120, hgt = 80;  // fits the image area of every panel below
    std::string start = R"({"type":"image.start","stream":3,"format":"rgb565","width":)" + std::to_string(w) +
                        R"(,"height":)" + std::to_string(hgt) + R"(,"ttl_s":30})";
    server(start.c_str());
    image_frames(app, w, hgt, 3, image_chunk);
    server(R"({"type":"image.end","stream":3})");
    advance(300);
    observe("image");
  }
  server(R"({"type":"error","text":"Model quota exceeded"})");
  advance(300);
  observe("error");
  app.console("settings");
  advance(300);
  observe("settings");
  app.console("settings close");
  advance(300);
  observe("settings-closed");
  app.on_transport_closed("lost");
  advance(500);
  observe("offline");
}

std::vector<uint32_t> session_hashes(Panel p) {
  RenderHal hal(p);
  std::vector<uint32_t> out;
  run_session(hal, 4096, [&](const char*) { out.push_back(fnv(hal.fb)); });
  return out;
}

void check_golden(Panel p, const std::vector<uint32_t>& golden) {
  std::vector<uint32_t> got = session_hashes(p);
  if (got != golden) {
    std::printf("  %dx%d%s hashes: {", p.width, p.height, p.round ? " round" : "");
    for (size_t i = 0; i < got.size(); ++i) std::printf("%s0x%08xu", i ? ", " : "", got[i]);
    std::printf("}\n");
  }
  CHECK(got == golden);
}

}  // namespace

TEST("render: rectangular 320x240 session matches the pinned pixels") {
  check_golden({320, 240, false},
               {
      0xb075112du, 0xb4d6babau, 0x42906324u, 0xe853056bu, 0xe853056bu, 0xe853056bu, 0xe853056bu,
      0xe853056bu, 0x2aca3ebbu, 0x2aca3ebbu, 0x2aca3ebbu, 0x2aca3ebbu, 0x2aca3ebbu, 0x2aca3ebbu,
      0x2aca3ebbu, 0x2aca3ebbu, 0xd264717du, 0xcbc8e2afu, 0x1e67af83u, 0x738c6c8fu, 0xd264717du,
      0xdf2603e3u, 0xa0c079d6u, 0xed53c3f1u, 0xf1cd192au, 0xe704069eu, 0x0db5a385u, 0x50b7e389u,
      0x81182519u, 0x27aad381u, 0x3d296a2bu, 0xf904483du, 0x9cd3e865u, 0x3571e0bcu, 0x113167d4u,
      0xa1e24fbcu, 0x9ed5ec87u, 0x9ed5ec87u, 0x824af1e4u, 0xa233fe39u, 0x79751efau
               });
}

TEST("render: round 466 session matches the pinned pixels") {
  check_golden({466, 466, true},
               {
      0xb342ac13u, 0xcc878abbu, 0x4249d0d9u, 0xfd8a15f1u, 0xfd8a15f1u, 0xfd8a15f1u, 0xfd8a15f1u,
      0xfd8a15f1u, 0xe11af4b1u, 0xe11af4b1u, 0xe11af4b1u, 0xe11af4b1u, 0xe11af4b1u, 0xe11af4b1u,
      0xe11af4b1u, 0xe11af4b1u, 0xf77d2b12u, 0xa5f774c6u, 0xde132a0au, 0xbd7c3d3au, 0xf77d2b12u,
      0x0d216f82u, 0x711d6a6cu, 0x567401f7u, 0x2d95bc70u, 0xee66d664u, 0x33193c71u, 0xac9cc72du,
      0xf7f85391u, 0x0f0e90cdu, 0xaef02f06u, 0x914ba318u, 0x914ba318u, 0xf52d7121u, 0x9e9c5389u,
      0x20bae321u, 0x0820eff0u, 0x0820eff0u, 0xb5bb87cdu, 0xa67fd05cu, 0x1568d8bbu
               });
}

TEST("render: square 480x480 session matches the pinned pixels") {
  check_golden({480, 480, false},
               {
      0xc8f5f917u, 0x3b67d88fu, 0x80cda567u, 0x837e9cbeu, 0x837e9cbeu, 0x837e9cbeu, 0x837e9cbeu,
      0x837e9cbeu, 0x5f86a42du, 0x5f86a42du, 0x5f86a42du, 0x5f86a42du, 0x5f86a42du, 0x5f86a42du,
      0x5f86a42du, 0x5f86a42du, 0xe91dbe22u, 0x0c2a742eu, 0xd76e692au, 0xaa44276au, 0xe91dbe22u,
      0x5fac3f2au, 0x91949bd5u, 0x629f78b2u, 0x7a5e6029u, 0xf309694du, 0xc0a2f345u, 0x8354b9b9u,
      0xbd7bd8f5u, 0xc1be2f89u, 0x6aac9baeu, 0xe97fe054u, 0xe97fe054u, 0x0487989fu, 0x265d620eu,
      0x2329abd7u, 0x73e85ce9u, 0x73e85ce9u, 0xa19de6c1u, 0x81cf77ebu, 0x1f4553afu
               });
}

TEST("render: small 240x240 session matches the pinned pixels") {
  check_golden({240, 240, false},
               {
      0x3eefe7bfu, 0x6dc4bbcbu, 0x80bc812cu, 0xf8917732u, 0xf8917732u, 0xf8917732u, 0xf8917732u,
      0xf8917732u, 0x98109559u, 0x98109559u, 0x98109559u, 0x98109559u, 0x98109559u, 0x98109559u,
      0x98109559u, 0x98109559u, 0x17a3c62eu, 0x95d2c89au, 0x44cde5f6u, 0x7f6fb0d6u, 0x17a3c62eu,
      0x91453856u, 0x3b959769u, 0x00efe3c6u, 0xcae0630du, 0x45e25fc1u, 0xa8e38869u, 0xd320839du,
      0xaf097f59u, 0xaff3592du, 0xb621436au, 0x54e3995cu, 0x54e3995cu, 0xcafa892cu, 0x49292419u,
      0x0ec45794u, 0x0fccd2edu, 0x0fccd2edu, 0xda8903b5u, 0x6b82d4fbu, 0x960e4bbbu
               });
}

namespace {

// Every frame of the session drawn in strips must equal the framebuffer
// rendering with the same row alignment, pixel for pixel.
void check_strips_match(Panel p, std::initializer_list<int> strip_rows, std::initializer_list<size_t> chunks) {
  RenderHal ref(p);
  std::vector<std::vector<uint16_t>> frames;
  std::vector<std::string> names;
  run_session(ref, 4096, [&](const char* name) {
    frames.push_back(ref.fb);
    names.emplace_back(name);
  });
  for (int rows : strip_rows) {
    for (size_t chunk : chunks) {
      Panel sp = p;
      sp.strip_rows = rows;
      RenderHal hal(sp);
      size_t step = 0;
      int mismatches = 0;
      run_session(hal, chunk, [&](const char* name) {
        if (step < frames.size() && hal.fb != frames[step] && mismatches++ == 0) {
          std::printf("  %dx%d%s align %d strips %d chunk %zu: first difference at step %zu (%s)\n", p.width, p.height,
                      p.round ? " round" : "", p.row_align, rows, chunk, step, name);
        }
        ++step;
      });
      CHECK_EQ(step, frames.size());
      CHECK_EQ(mismatches, 0);
      CHECK_EQ(hal.bad_presents, 0);
      CHECK(hal.strips > 0);
    }
  }
}

}  // namespace

TEST("render: strips reproduce the framebuffer on a 480x480 panel with even windows") {
  check_strips_match({480, 480, false, 2}, {2, 8, 24, 480}, {4096, 1000, 778});
}

TEST("render: strips reproduce the framebuffer with odd strip heights and no alignment") {
  check_strips_match({320, 240, false, 1}, {1, 7, 24}, {4096, 778});
  check_strips_match({240, 240, false, 1}, {13}, {1000});
}

TEST("render: strips keep a round panel's border dark") {
  check_strips_match({466, 466, true, 2}, {2, 24}, {4096, 778});
}

TEST("render: row alignment only moves band edges onto even rows") {
  // With alignment 1 a 480x480 layout is unchanged; with 2 every band edge is even.
  RenderHal plain({480, 480, false, 1}), even({480, 480, false, 2});
  hg::Ui a(plain), b(even);
  CHECK_EQ(a.layout().scale, b.layout().scale);
  CHECK(b.layout().top_h % 2 == 0 && b.layout().header_h % 2 == 0 && b.layout().bottom_h % 2 == 0);
  CHECK(b.layout().main_y % 2 == 0 && b.layout().main_h % 2 == 0);
}
