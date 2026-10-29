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
//
// Registered as an AsyncWebHandler on ESPHome's web server, which works on
// every chip: ESP32 builds use ESPHome's own ESP-IDF web server (no
// `server->on()`, no `_tempObject`), ESP8266 and LibreTiny the Arduino
// ESPAsyncWebServer -- both call canHandle(), handleBody() for each chunk
// of a text/plain body, then handleRequest() once it's all in.
class AstrionHttpEndpoint : public esphome::Component, public AsyncWebHandler {
 public:
  // Set from YAML (on_boot lambda), where `id(play_pronto_script)` is
  // valid. `id()` is ESPHome's YAML-codegen macro, not a real C++ symbol
  // -- it can't be used directly inside this plain .h file, which is why
  // the script pointer is injected from the outside instead.
  // Script parameters: pronto_code, delay_ms, out.
  // Plain `int`: ESPHome declares a script `int` parameter as C++ `int`, which isn't
  // int32_t on every chip (on BK7231N, int32_t is `long`, a different type).
  using PlayScript = esphome::script::Script<std::string, int, int>;
  void set_script(PlayScript *script) { this->script_ = script; }

  void setup() override {
    if (esphome::web_server_base::global_web_server_base == nullptr) {
      ESP_LOGE("astrion_api", "web_server_base not available -- is `web_server:` configured?");
      return;
    }
    esphome::web_server_base::global_web_server_base->add_handler(this);
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_POST) return false;
#ifdef USE_ESP32
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url_buf) == "/pronto";
#else
    return request->url() == "/pronto";
#endif
  }

  // Not trivial: this handler needs the request body.
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool isRequestHandlerTrivial() const override { return false; }

  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    // One request at a time is assumed (the IR codes come from one remote):
    // a new body (index 0) starts over.
    if (index == 0) this->body_.clear();
    this->body_.append(reinterpret_cast<const char *>(data), len);
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleRequest(AsyncWebServerRequest *request) override {
    // IMPORTANT: this runs on the web server's own task, NOT the main
    // ESPHome loop -- calling straight into transmitter/logger/script APIs
    // from here corrupted output in testing (garbled log tags, stray bytes,
    // truncated strings) from the two threads touching shared state at the
    // same time. Only queue the data here; the actual transmission happens
    // in loop() below, on the right thread.
    Request item;
    item.body.swap(this->body_);
    if (request->hasParam("out")) {
      item.out = atoi(request->getParam("out")->value().c_str());
    }
    if (item.body.find_first_not_of(" \t\r\n") == std::string::npos) {
      request->send(400, "text/plain", "Send the Pronto code(s) as the body, Content-Type: text/plain\n");
      return;
    }
    if (!this->enqueue(item)) {
      ESP_LOGW("astrion_api", "Pronto queue full (%d pending) -- dropping this request", (int) QUEUE_CAPACITY - 1);
      request->send(503, "text/plain", "Busy - Pronto queue full\n");
      return;
    }
    request->send(200, "text/plain", "OK - Pronto Code(s) Queued\n");
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
      int delay_ms = 0;
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
    int out{0};
  };

  PlayScript *script_{nullptr};
  std::string body_;  // the request body being received, see handleBody()
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
