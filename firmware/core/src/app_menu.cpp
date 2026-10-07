// Hermes's menu: lists the server sends (status, model and session pickers),
// shown as a Menu screen and picked with touch or the buttons. The device only
// renders and reports picks; what an item means is up to the server.
#include <algorithm>

#include "hg/app.hpp"

namespace hg {
namespace {

constexpr uint32_t kMenuWaitMs = 20000;  // a pick may switch models (a slow provider lookup)
constexpr size_t kMaxMenuItems = 64;

}  // namespace

bool App::open_menu() {
  if (prompt_showing() || ota_busy() || ota_ == Ota::Restarting) return false;
  if (!online() || !paired_) {
    set_hint_flash(online() ? "Approve pairing first" : "Not connected to Hermes");
    update_model();
    return false;
  }
  wake_display();
  close_wifi_setup();
  if (settings_open()) close_settings();
  if (mode_ == Mode::Listening) cancel_listening("menu");
  dismiss_overlay();
  menu_reset();
  menu_active_ = true;
  menu_loading_ = true;
  menu_wait_until_ = now() + kMenuWaitMs;
  menu_title_ = "Hermes";
  json::Value msg = json::Value::object();
  msg.set("type", "menu.open").set("menu", "main");
  send(msg);
  update_model();
  return true;
}

void App::menu_reset() {
  menu_active_ = menu_loading_ = false;
  menu_id_.clear();
  menu_title_.clear();
  menu_items_.clear();
  menu_cursor_ = menu_top_ = 0;
}

void App::close_menu() {
  if (!menu_active_) return;
  if (!menu_id_.empty() && online()) {
    json::Value msg = json::Value::object();
    msg.set("type", "menu.close").set("id", menu_id_);
    send(msg);
  }
  menu_reset();
  update_model();
}

std::vector<App::MenuEntry> App::menu_entries() const {
  std::vector<MenuEntry> rows = menu_items_;
  // Boards without UP/DOWN step through the rows with CANCEL, so the list needs a way out.
  if (!rows.empty() && !profile_.has_scroll_buttons) rows.push_back({"", "Close", "", false});
  return rows;
}

void App::menu_move(int delta) {
  const int n = static_cast<int>(menu_entries().size());
  if (n == 0) return;
  menu_cursor_ = ((menu_cursor_ + delta) % n + n) % n;
  const int rows = ui_ ? ui_->menu_rows() : n;
  if (menu_cursor_ < menu_top_) menu_top_ = menu_cursor_;
  if (menu_cursor_ >= menu_top_ + rows) menu_top_ = menu_cursor_ - rows + 1;
}

void App::menu_page() {
  const int n = static_cast<int>(menu_entries().size());
  const int rows = ui_ ? ui_->menu_rows() : n;
  if (!menu_active_ || n <= rows) return;
  menu_top_ += rows;
  if (menu_top_ >= n) menu_top_ = 0;
  menu_top_ = std::min(menu_top_, n - rows);
  menu_cursor_ = menu_top_;
  update_model();
}

void App::menu_pick(int index) {
  const std::vector<MenuEntry> rows = menu_entries();
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  menu_cursor_ = index;
  if (rows[static_cast<size_t>(index)].id.empty()) {
    close_menu();
    return;
  }
  if (menu_loading_) return;  // one pick at a time
  json::Value msg = json::Value::object();
  msg.set("type", "menu.select").set("id", menu_id_).set("item", rows[static_cast<size_t>(index)].id);
  send(msg);
  menu_loading_ = true;
  menu_wait_until_ = now() + kMenuWaitMs;
}

bool App::menu_tap(int x, int y) {
  if (!menu_open() || !ui_) return false;
  const int row = ui_->menu_hit(x, y);
  if (row < 0 || menu_top_ + row >= static_cast<int>(menu_entries().size())) return false;
  menu_pick(menu_top_ + row);
  update_model();
  return true;
}

void App::menu_input(Button button, bool pressed) {
  if (pressed) {
    if (button == Button::Up) menu_move(-1);
    if (button == Button::Down) menu_move(+1);
    return;
  }
  if (button == Button::Talk) menu_pick(menu_cursor_);
  else if (button == Button::Cancel) {
    if (profile_.has_scroll_buttons) close_menu();
    else menu_move(+1);
  }
}

void App::menu_tick() {
  if (menu_loading_ && static_cast<int32_t>(now() - menu_wait_until_) >= 0) {
    menu_loading_ = false;
    if (menu_items_.empty()) menu_reset();
    notice_ = "Hermes did not answer";
    notice_until_ = now() + 8000;
    update_model();
  }
}

void App::h_menu(const json::Value& m) {
  const std::string& id = m["id"].as_string();
  if (id.empty()) return;
  close_wifi_setup();
  wake_display();
  if (settings_open()) close_settings();
  dismiss_overlay();
  menu_items_.clear();
  int current = -1;
  for (const json::Value& item : m["items"].elements()) {
    if (menu_items_.size() >= kMaxMenuItems) break;
    MenuEntry e{item["id"].as_string(), item["label"].as_string(), item["note"].as_string(),
                item["current"].as_bool()};
    if (e.id.empty()) continue;
    if (e.current && current < 0) current = static_cast<int>(menu_items_.size());
    menu_items_.push_back(std::move(e));
  }
  menu_active_ = true;
  menu_loading_ = false;
  menu_id_ = id;
  menu_title_ = m["title"].as_string();
  menu_cursor_ = menu_top_ = 0;
  menu_move(std::max(0, current));
}

void App::h_menu_close(const json::Value& m) {
  const std::string& id = m["id"].as_string();
  if (menu_active_ && (id.empty() || id == menu_id_ || menu_id_.empty())) menu_reset();
}

void App::h_info(const json::Value& m) {
  info_model_ = m["model"].as_string();
  info_session_ = m["session"].as_string();
}

void App::menu_model() {
  UiModel& m = model_;
  m.screen = Screen::Menu;
  m.headline = menu_title_.empty() ? "Hermes" : menu_title_;
  m.scroll = 0;
  m.speaking = false;
  m.rows.clear();
  for (const MenuEntry& e : menu_entries()) m.rows.push_back({e.label, e.note, e.current});
  if (m.rows.empty()) m.rows.push_back({"Loading...", "", false});
  m.row_top = menu_top_;
  m.row_cursor = menu_items_.empty() ? -1 : menu_cursor_;
  const bool pages = ui_ && static_cast<int>(m.rows.size()) > ui_->menu_rows();
  if (menu_loading_) m.hint = "Loading...";
  else if (!notice_.empty()) m.hint = notice_;
  else if (profile_.touch_screen) m.hint = pages ? "Tap pick  Swipe up: more" : "Tap: pick  Swipe: close";
  else if (profile_.has_scroll_buttons) m.hint = profile_.talk_label + ": pick  " + profile_.cancel_label + ": close";
  else m.hint = profile_.talk_label + ": pick  " + profile_.cancel_label + ": next";
}

}  // namespace hg
