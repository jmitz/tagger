#include "http_worker.h"

#include "esp_http_client.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tagger_app {

static const char *const TAG = "tagger.http";
static const size_t MAX_BODY = 512 * 1024;  // generous: lists can be large, PSRAM backs big allocations

void HttpWorker::start() {
  if (this->jobs_ != nullptr)
    return;
  this->jobs_ = xQueueCreate(16, sizeof(Job *));
  this->done_ = xQueueCreate(16, sizeof(Done *));
  xTaskCreatePinnedToCore(&HttpWorker::task_entry_, "spoolman_http", 8192, this, 4, nullptr, 0);
}

void HttpWorker::set_base_url(const std::string &url) {
  std::string u = url;
  if (u.find("://") == std::string::npos)
    u = "http://" + u;
  while (!u.empty() && u.back() == '/')
    u.pop_back();
  this->base_url_ = u;
}

void HttpWorker::request(const char *method, const std::string &path, const std::string &body, HttpCallback cb) {
  auto *job = new Job{this->next_id_++, method, this->base_url_ + path, body};
  this->pending_[job->id] = std::move(cb);
  ESP_LOGD(TAG, "-> %s %s %s", method, path.c_str(), body.c_str());
  if (xQueueSend(this->jobs_, &job, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Request queue full, dropping %s %s", method, path.c_str());
    HttpResult r;
    r.error = "queue full";
    auto it = this->pending_.find(job->id);
    HttpCallback c = std::move(it->second);
    this->pending_.erase(it);
    delete job;
    c(r);
  }
}

void HttpWorker::poll() {
  Done *done = nullptr;
  while (this->done_ != nullptr && xQueueReceive(this->done_, &done, 0) == pdTRUE) {
    this->last_status_ = done->result.status;
    auto it = this->pending_.find(done->id);
    if (it != this->pending_.end()) {
      HttpCallback cb = std::move(it->second);
      this->pending_.erase(it);
      cb(done->result);
    }
    delete done;
  }
}

void HttpWorker::task_entry_(void *arg) { static_cast<HttpWorker *>(arg)->run_(); }

void HttpWorker::run_() {
  for (;;) {
    Job *job = nullptr;
    if (xQueueReceive(this->jobs_, &job, portMAX_DELAY) != pdTRUE)
      continue;
    auto *done = new Done{job->id, perform_(*job)};
    delete job;
    xQueueSend(this->done_, &done, portMAX_DELAY);
  }
}

HttpResult HttpWorker::perform_(const Job &job) {
  HttpResult res;
  esp_http_client_config_t cfg = {};
  cfg.url = job.url.c_str();
  cfg.timeout_ms = 6000;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  esp_http_client_method_t m = HTTP_METHOD_GET;
  if (job.method == "POST")
    m = HTTP_METHOD_POST;
  else if (job.method == "PUT")
    m = HTTP_METHOD_PUT;
  else if (job.method == "PATCH")
    m = HTTP_METHOD_PATCH;
  else if (job.method == "DELETE")
    m = HTTP_METHOD_DELETE;
  cfg.method = m;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    res.error = "init failed";
    return res;
  }
  if (!job.body.empty())
    esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");

  esp_err_t err = esp_http_client_open(client, job.body.size());
  if (err != ESP_OK) {
    res.error = esp_err_to_name(err);
    esp_http_client_cleanup(client);
    return res;
  }
  if (!job.body.empty() &&
      esp_http_client_write(client, job.body.data(), job.body.size()) != (int) job.body.size()) {
    res.error = "write failed";
    esp_http_client_cleanup(client);
    return res;
  }
  int64_t len = esp_http_client_fetch_headers(client);
  res.status = esp_http_client_get_status_code(client);
  if (len > 0)
    res.body.reserve(std::min<int64_t>(len, MAX_BODY));
  char buf[1024];
  for (;;) {
    int n = esp_http_client_read(client, buf, sizeof(buf));
    if (n <= 0)
      break;
    if (res.body.size() + n > MAX_BODY) {
      res.error = "response too large";
      break;
    }
    res.body.append(buf, n);
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return res;
}

}  // namespace tagger_app
}  // namespace esphome
