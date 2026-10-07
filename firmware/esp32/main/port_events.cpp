#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace hgp::events {
namespace {

const char* TAG = "hg.events";
QueueHandle_t queue = nullptr;
// Payload bytes posted but not yet released. Without PSRAM a burst of image
// chunks could otherwise take all internal RAM: producers wait for the app.
std::atomic<size_t> in_flight{0};
constexpr size_t kBudget = static_cast<size_t>(CONFIG_HG_EVENT_BYTES_KB) * 1024;

}  // namespace

void init() { queue = xQueueCreate(48, sizeof(Event)); }

bool post(EventType type, const void* data, size_t len, uint32_t generation, ConsoleRequest* console) {
  Event ev{type, generation, nullptr, len, console};
  if (kBudget && data && len) {
    for (int waited = 0; in_flight.load() + len > kBudget && in_flight.load() > 0 && waited < 50; ++waited) {
      vTaskDelay(pdMS_TO_TICKS(10));  // up to 0.5 s, then post anyway
    }
  }
  if (data && len) {
    // Payloads may be large (WebSocket frames): prefer PSRAM when present.
    ev.data = static_cast<uint8_t*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ev.data) ev.data = static_cast<uint8_t*>(malloc(len + 1));
    if (!ev.data) {
      ESP_LOGW(TAG, "out of memory for a %u byte event", static_cast<unsigned>(len));
      return false;
    }
    std::memcpy(ev.data, data, len);
    ev.data[len] = 0;
    in_flight += len;
  }
  if (xQueueSend(queue, &ev, pdMS_TO_TICKS(20)) != pdTRUE) {
    ESP_LOGW(TAG, "event queue full; dropping event %d", static_cast<int>(type));
    release(ev);
    return false;
  }
  return true;
}

bool receive(Event& out, TickType_t wait) { return xQueueReceive(queue, &out, wait) == pdTRUE; }

void release(Event& ev) {
  if (ev.data) in_flight -= ev.len;
  free(ev.data);
  ev.data = nullptr;
}

}  // namespace hgp::events
