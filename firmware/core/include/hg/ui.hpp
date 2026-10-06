// Screen model and renderer.
//
// The app fills a UiModel; Ui renders it into the framebuffer in horizontal
// bands and flushes only the bands whose inputs changed. The renderer is
// deterministic (no clocks, no randomness): the same model always produces the
// same pixels, which is what the simulator's screenshot tests rely on.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/hal.hpp"
#include "hg/mascot.hpp"
#include "hg/qr.hpp"

namespace hg {

enum class Screen : uint8_t {
  Boot,
  Offline,
  Connecting,
  Pairing,
  Ready,
  Listening,
  Thinking,
  Responding,
  Card,
  Image,
  Error,
  Prompt,  // a yes/no question from Hermes, answered with the buttons
  Updating,  // installing a firmware update
  Settings,
  Setup,
};

enum class Link : uint8_t { Offline, Network, Connecting, Online };

const char* screen_name(Screen s);

struct UiModel {
  Screen screen = Screen::Boot;
  Link link = Link::Offline;
  std::string title;     // top bar (device name)
  std::string headline;  // header band, next to the indicator
  std::string detail;    // secondary line (status phrase, URL, error)
  std::string body;      // main text (reply, card body)
  std::string code;      // pairing code
  int scroll = -1;       // first visible body line; -1 pins to the end
  uint8_t level = 0;     // microphone level 0..100
  bool speaking = false;
  bool color_test = false;
  uint32_t frame = 0;    // animation frame, advanced by the app
  std::string hint;      // bottom bar
  std::string yes, no;   // answer buttons under the hero caption (Prompt screen)
  std::string qr;        // Setup screen: text to show as a QR code above detail and body
  // Show the mascot as large as fits, with headline/detail as a caption,
  // instead of the header + text layout.
  bool hero = false;
  uint8_t caption_lines = 1;  // hero: lines the detail may wrap to (then "..")
};

struct UiLayout {
  int scale = 1;  // base text scale
  int top_h = 0, header_h = 0, bottom_h = 0;
  int body_cols = 0, body_rows = 0;
  int main_y = 0, main_h = 0;  // area between the bars (used for images)
  int hero_cols = 0;  // characters per hero caption line
};

class Ui {
 public:
  explicit Ui(Display& display);

  // Renders bands whose inputs changed since the last call and flushes them.
  void render(const UiModel& model);
  // Forces a full redraw on the next render (after something else drew).
  void invalidate();
  const UiLayout& layout() const { return layout_; }
  // The area the UI draws in: the whole panel, or on a round panel the square
  // inscribed in it. Layout coordinates are relative to this area.
  const DisplayInfo& area() const { return info_; }
  // A canvas over that area, and a flush of its rows [y0, y1). Framebuffer
  // displays only; strip displays draw through paint().
  Canvas canvas();
  void flush(int y0, int y1);
  // The display has no framebuffer and is drawn in strips.
  bool strips() const { return panel_.strip_rows > 0; }
  // Band edges sit on multiples of this; strips are sent in whole units.
  int row_align() const { return align_; }
  // Draws rows [y0, y1) of the area with `draw(Canvas&)` (clipped to those
  // rows) and sends them to the panel. With a framebuffer this is one pass;
  // in strip mode `draw` runs once per strip, must repaint every pixel of the
  // rows it is given, and the range grows to row_align() boundaries.
  template <typename Draw>
  void paint(int y0, int y1, Draw&& draw) {
    if (!strips()) {
      Canvas c = canvas();
      c.set_clip_rows(y0, y1);
      draw(c);
      flush(y0, y1);
      return;
    }
    paint_strips(y0, y1, [&](Canvas& c) { draw(c); });
  }
  // Body text rows visible on a text screen for this model (after detail lines).
  int body_rows(const UiModel& m) const;
  bool title_hit(int x, int y) const {
    return x >= ox_ && x < ox_ + info_.width && y >= oy_ && y < oy_ + layout_.top_h;
  }

 private:
  void draw_top(Canvas& c, const UiModel& m);
  void draw_header(Canvas& c, const UiModel& m);
  void draw_content(Canvas& c, const UiModel& m);
  void draw_qr(Canvas& c, const UiModel& m, int y0, int y1);
  void draw_bottom(Canvas& c, const UiModel& m);
  void draw_indicator(Canvas& c, const UiModel& m, int cx, int cy, int r);
  struct HeroGeom {
    int size = 0, x = 0, y = 0;  // mascot
    int caption_y = 0, buttons_y = 0;
    std::vector<std::string> detail;  // wrapped, already truncated
  };
  HeroGeom hero_geom(const UiModel& m) const;
  void draw_hero(Canvas& c, const UiModel& m);
  // Rows [y0, y1) that hero animations may touch for this screen.
  void hero_anim_rows(const UiModel& m, int& y0, int& y1) const;
  // Strip-mode paint(): `draw` gets a canvas over the area for each strip.
  template <typename Draw>
  void paint_strips(int y0, int y1, Draw&& draw);
  // Fills the whole panel with the background (round panels: outside the area).
  void paint_panel_background();
  // Strip mode on a round panel: background outside the area for one strip.
  void paint_border(uint16_t* buf, int y, int rows);

  Display& display_;
  DisplayInfo panel_;
  DisplayInfo info_;
  int ox_ = 0, oy_ = 0;  // area origin on the panel
  int align_ = 1;        // row granularity on the panel (DisplayInfo::row_align)
  UiLayout layout_;
  uint32_t hash_[4] = {0, 0, 0, 0};
  uint32_t hero_static_ = 0, hero_anim_ = 0;
  std::string qr_text_;  // the code below encodes this
  qr::Code qr_code_;
  bool hero_valid_ = false;
  bool valid_ = false;
};

template <typename Draw>
void Ui::paint_strips(int y0, int y1, Draw&& draw) {
  const int a = align_, rows = panel_.strip_rows;
  // Panel rows, grown to whole alignment units and kept on the panel.
  int p0 = (y0 + oy_) / a * a;
  int p1 = std::min<int>(panel_.height, (y1 + oy_ + a - 1) / a * a);
  const bool border = ox_ || oy_;
  for (int y = p0; y < p1; y += rows) {
    const int n = std::min(rows, p1 - y);
    uint16_t* buf = display_.strip(static_cast<uint16_t>(y));
    if (!buf) return;
    if (border) paint_border(buf, y, n);
    Canvas c(buf + ox_, info_.width, info_.height, info_.swap_bytes, panel_.width, y - oy_, n);
    c.set_clip_rows(y - oy_, y - oy_ + n);
    draw(c);
    display_.present(static_cast<uint16_t>(y), static_cast<uint16_t>(y + n));
  }
}

}  // namespace hg
