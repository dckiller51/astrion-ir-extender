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
class AstrionHttpEndpoint : public esphome::Component {
 public:
  // Set from YAML (on_boot lambda), where `id(play_pronto_script)` is
  // valid. `id()` is ESPHome's YAML-codegen macro, not a real C++ symbol
  // -- it can't be used directly inside this plain .h file, which is why
  // the script pointer is injected from the outside instead.
  void set_script(esphome::script::Script<std::string> *script) { this->script_ = script; }

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
            request->_tempObject = new std::string();
          }
          auto *buffer = static_cast<std::string *>(request->_tempObject);
          buffer->append(reinterpret_cast<const char *>(data), len);

          if (index + len == total) {
            // IMPORTANT: this callback runs on AsyncTCP's own task, NOT
            // the main ESPHome loop -- calling straight into
            // transmitter/logger/script APIs from here corrupted output
            // in testing (garbled log tags, stray bytes, truncated
            // strings) from the two threads touching shared state at the
            // same time. Only hand off the data here; the actual
            // transmission happens in loop() below, on the right thread.
            this->pending_code_ = *buffer;
            this->has_pending_ = true;
            delete buffer;
            request->_tempObject = nullptr;
            request->send(200, "text/plain", "OK - Pronto Code Received\n");
          }
        });
  }

  void loop() override {
    if (!this->has_pending_) return;
    this->has_pending_ = false;

    if (this->script_ == nullptr) {
      ESP_LOGE("astrion_api", "Pronto code received but no script configured");
      return;
    }
    this->script_->execute(this->pending_code_);
  }

 protected:
  esphome::script::Script<std::string> *script_{nullptr};
  std::string pending_code_;
  // Simple single-writer (AsyncTCP callback) / single-reader (main loop)
  // handoff flag. Not a full mutex -- a torn read is theoretically
  // possible, but this is far safer than the direct cross-thread calls
  // it replaces, and matches the level of rigor typical of ESPHome
  // custom components for this kind of handoff.
  volatile bool has_pending_{false};
};