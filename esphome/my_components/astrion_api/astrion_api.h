#pragma once
#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/script/script.h"

// Accepts a full Pronto code as a raw POST body at /pronto -- unlike
// ESPHome's native `text:` entity (used by the earlier chunk+fire
// protocol this replaces), a raw AsyncWebServer body handler has no
// 255-character schema limit, so the whole code can be sent in one
// request.
//
// The body is one Pronto code, or a batch: one code per line, each
// optionally prefixed `<delay_ms>>` (wait that long after the previous
// line before sending this one). Each line becomes one run of the YAML's
// `play_pronto_script` (mode: queued), which does the waiting with a
// non-blocking `delay:` and keeps the lines in order.
//
// Optional query parameter `?out=N` picks the IR output on boards that
// have several (KC868-AGv3: 1 = built-in LEDs, 2 = extension cable 1,
// 3 = extension cable 2). 0 or absent = the board's default output; a
// board with a single output ignores it.
class AstrionHttpEndpoint : public esphome::Component {
 public:
  // Set from YAML (on_boot lambda), where `id(play_pronto_script)` is
  // valid. `id()` is ESPHome's YAML-codegen macro, not a real C++ symbol
  // -- it can't be used directly inside this plain .h file, which is why
  // the script pointer is injected from the outside instead.
  // Script parameters: pronto_code, delay_ms, out.
  using PlayScript = esphome::script::Script<std::string, int32_t, int32_t>;
  void set_script(PlayScript *script) { this->script_ = script; }

  void setup() override {
    if (esphome::web_server_base::global_web_server_base == nullptr) {
      ESP_LOGE("astrion_api", "web_server_base not available -- is `web_server:` configured?");
      return;
    }
    auto *server = esphome::web_server_base::global_web_server_base->get_server();
    if (server == nullptr) {
      ESP_LOGE("astrion_api", "AsyncWebServer instance not available");
      return;
    }

    server->on(
        "/pronto", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr,
        [this](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
          if (index == 0) {
            auto *item = new Request();
            if (request->hasParam("out")) {
              item->out = atoi(request->getParam("out")->value().c_str());
            }
            request->_tempObject = item;
          }
          auto *item = static_cast<Request *>(request->_tempObject);
          item->body.append(reinterpret_cast<const char *>(data), len);

          if (index + len == total) {
            // IMPORTANT: this callback runs on AsyncTCP's own task, NOT
            // the main ESPHome loop -- calling straight into
            // transmitter/logger/script APIs from here corrupted output
            // in testing (garbled log tags, stray bytes, truncated
            // strings) from the two threads touching shared state at the
            // same time. Only queue the data here; the actual
            // transmission happens in loop() below, on the right thread.
            bool queued = this->enqueue(*item);
            delete item;
            request->_tempObject = nullptr;
            if (!queued) {
              ESP_LOGW("astrion_api", "Pronto queue full (%d pending) -- dropping this request", (int) QUEUE_CAPACITY - 1);
              request->send(503, "text/plain", "Busy - Pronto queue full\n");
              return;
            }
            request->send(200, "text/plain", "OK - Pronto Code(s) Queued\n");
          }
        });
  }

  void loop() override {
    // DIAGNOSTIC, temporary: confirms whether loop() runs at all for a
    // component constructed+registered dynamically via on_boot (as
    // opposed to a normal statically-declared one) -- the queue staying
    // permanently full even after a full minute of idle time and a
    // reboot suggests it might not be. Remove once confirmed either way.
    uint32_t now = millis();
    if (now - this->last_heartbeat_ >= 5000) {
      this->last_heartbeat_ = now;
      ESP_LOGD("astrion_api", "loop() heartbeat, queue depth=%d", (int) ((this->tail_ - this->head_ + QUEUE_CAPACITY) % QUEUE_CAPACITY));
    }

    // Drains exactly one queued code per call. remote_transmitter's
    // transmit_pronto is a blocking, synchronous call (confirmed by
    // ESPHome's own "took a long time" warning during testing -- it
    // really does occupy the main loop for the whole transmission), so
    // this naturally serializes: the next queued code can't start until
    // this one has fully gone out, unlike the single-slot design it
    // replaces, which let a fast second POST overwrite a first one still
    // waiting to be sent (confirmed in practice: switching between two
    // Activities sharing this extender, with no gap between their
    // commands, silently dropped one device's command every time).
    Request next;
    if (!this->dequeue(&next)) return;

    if (this->script_ == nullptr) {
      ESP_LOGE("astrion_api", "Pronto code dequeued but no script configured");
      return;
    }
    // One script run per line; the script is `mode: queued`, so the lines
    // go out in order, each after its own delay.
    size_t start = 0;
    while (start <= next.body.size()) {
      size_t end = next.body.find('\n', start);
      if (end == std::string::npos) end = next.body.size();
      std::string line = next.body.substr(start, end - start);
      start = end + 1;
      int32_t delay_ms = 0;
      size_t mark = line.find('>');
      if (mark != std::string::npos) {
        delay_ms = atoi(line.substr(0, mark).c_str());
        line = line.substr(mark + 1);
      }
      size_t first = line.find_first_not_of(" \t\r");
      if (first == std::string::npos) continue;  // empty line
      size_t last = line.find_last_not_of(" \t\r");
      line = line.substr(first, last - first + 1);
      this->script_->execute(line, delay_ms < 0 ? 0 : delay_ms, next.out);
    }
  }

 protected:
  struct Request {
    std::string body;
    int32_t out{0};
  };

  PlayScript *script_{nullptr};
  uint32_t last_heartbeat_{0};

  // Classic lock-free single-producer (enqueue(), called from AsyncTCP's
  // task)/single-consumer (dequeue(), called from the main loop task)
  // ring buffer -- no std::mutex, no <atomic>: neither reliably compiles
  // across every ESPHome target this project supports (std::mutex
  // specifically does not exist on LibreTiny/BK7231N's standard library --
  // confirmed by a real build failure, not a guess). `volatile` indices
  // are enough here because the two sides never touch the same slot at
  // the same time: the producer only advances tail_ *after* writing a
  // slot, the consumer only advances head_ *after* reading one -- the
  // same technique Arduino's own core uses for its interrupt-driven
  // Serial RX buffer.
  static constexpr size_t QUEUE_CAPACITY = 8;
  Request queue_[QUEUE_CAPACITY];
  volatile size_t head_{0};
  volatile size_t tail_{0};

  bool enqueue(const Request &item) {
    size_t next_tail = (this->tail_ + 1) % QUEUE_CAPACITY;
    if (next_tail == this->head_) return false; // full
    this->queue_[this->tail_] = item;
    this->tail_ = next_tail;
    return true;
  }

  bool dequeue(Request *out) {
    if (this->head_ == this->tail_) return false; // empty
    *out = this->queue_[this->head_];
    this->head_ = (this->head_ + 1) % QUEUE_CAPACITY;
    return true;
  }
};
