// Background HTTP client for the Spoolman REST API.
//
// Requests are queued from the main (LVGL) loop and executed on a dedicated
// FreeRTOS task, so a slow or unreachable server never freezes the touch UI.
// Completed responses are queued back and their callbacks run from poll(),
// which the owning component calls from loop() - i.e. on the LVGL thread.
#pragma once

#include <functional>
#include <map>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace esphome {
namespace tagger_app {

struct HttpResult {
  int status{-1};       // HTTP status, or -1 when the request never completed
  std::string body;     // response body (may be empty)
  std::string error;    // transport error text when status == -1
  bool ok() const { return status >= 200 && status < 300; }
};

using HttpCallback = std::function<void(const HttpResult &)>;

class HttpWorker {
 public:
  void start();
  void set_base_url(const std::string &url);
  const std::string &base_url() const { return this->base_url_; }

  // method: GET/POST/PUT/PATCH/DELETE; path is appended to the base URL
  // (e.g. "/api/v1/spool/3"). body is sent as application/json when non-empty.
  void request(const char *method, const std::string &path, const std::string &body, HttpCallback cb);

  // Runs callbacks for finished requests. Call from the main loop only.
  void poll();

  bool busy() const { return !this->pending_.empty(); }
  int last_status() const { return this->last_status_; }

 protected:
  struct Job {
    uint32_t id;
    std::string method, url, body;
  };
  struct Done {
    uint32_t id;
    HttpResult result;
  };
  static void task_entry_(void *arg);
  void run_();
  static HttpResult perform_(const Job &job);

  std::string base_url_;
  QueueHandle_t jobs_{nullptr};
  QueueHandle_t done_{nullptr};
  uint32_t next_id_{1};
  std::map<uint32_t, HttpCallback> pending_;
  int last_status_{0};
};

}  // namespace tagger_app
}  // namespace esphome
