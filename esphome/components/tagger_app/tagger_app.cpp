#include "tagger_app.h"

#include <algorithm>
#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace tagger_app {

static const char *const TAG = "tagger";
static const uint32_t HEALTH_INTERVAL_MS = 30000;

// Palette
static const uint32_t C_BG = 0x101418, C_CARD = 0x1E252C, C_TEXT = 0xE8ECEF, C_DIM = 0x8FA3B3,
                      C_SUB = 0xB8C2CA, C_OK = 0x2E7D4F, C_WARN = 0xC77D1E, C_ERR = 0xC0392B,
                      C_BLUE = 0x2F6FDE, C_IDLE = 0x303A44;

// ------------------------------------------------------------------ lifecycle
void TaggerApp::setup() {
  this->http_.start();
  ESP_LOGCONFIG(TAG, "Spoolman: %s", this->http_.base_url().c_str());
}

void TaggerApp::loop() {
  this->http_.poll();
  uint32_t now = millis();
  if (now - this->last_health_ > HEALTH_INTERVAL_MS || this->last_health_ == 0) {
    this->last_health_ = now == 0 ? 1 : now;
    this->check_health_();
  }
  // Retry failed tag lookups once Spoolman is reachable again.
  for (int i = 0; i < 2; i++) {
    TagSlot &s = this->slots_[i];
    if (s.present && s.spool_id < 0 && !s.looking && this->online_)
      this->lookup_tag_(i);
  }
  if (this->spool_.valid() && !this->sending_ && !this->fetching_spool_ && now - this->last_refresh_ > 5000) {
    this->last_refresh_ = now;
    this->refresh_spool_();
  }
  if (this->new_armed_until_ != 0 && now > this->new_armed_until_) {
    this->new_armed_until_ = 0;
    this->dirty_ = true;
  }
  if (this->dirty_ && this->ui_built_) {
    this->dirty_ = false;
    this->render_();
  }
}

// ------------------------------------------------------------------ inputs
void TaggerApp::on_tag(int reader, const std::string &uid, const std::string &format) {
  if (reader < 0 || reader > 1)
    return;
  std::string u = norm_uid(uid);
  if (u.empty())
    return;
  // Already latched (same tag re-read, or seen by the other reader too): ignore.
  for (auto &s : this->slots_)
    if (s.present && s.uid == u)
      return;
  int slot = !this->slots_[0].present ? 0 : !this->slots_[1].present ? 1 : -1;
  if (slot < 0) {
    ESP_LOGD(TAG, "Reader %d: tag %s ignored, two tags already latched", reader + 1, u.c_str());
    return;
  }
  TagSlot &s = this->slots_[slot];
  s = TagSlot{};
  s.present = true;
  s.reader = reader;
  s.uid = u;
  s.format = format;
  ESP_LOGI(TAG, "Tag %d latched from reader %d: %s (%s)", slot + 1, reader + 1, u.c_str(), format.c_str());
  if (slot == 0) {
    this->loaded_since_latch_ = !std::isnan(this->live_) && this->live_ > this->reset_threshold_;
    this->empty_since_ = 0;
    this->set_status_("");
  }
  this->reset_capture_();
  this->lookup_tag_(slot);
  this->resolve_();
}

void TaggerApp::on_tag_removed(int reader) {
  // Tags are latched: a tag leaving a reader's field does not clear it.
  ESP_LOGD(TAG, "Reader %d: tag left field (latched tags kept)", reader + 1);
}

void TaggerApp::clear_() {
  ESP_LOGI(TAG, "Clearing latched tags");
  this->slots_[0] = TagSlot{};
  this->slots_[1] = TagSlot{};
  this->loaded_since_latch_ = false;
  this->empty_since_ = 0;
  this->reset_capture_();
  this->set_status_("");
  this->resolve_();
}

void TaggerApp::on_weight(float grams) {
  if (std::isnan(grams))
    return;
  uint32_t now = millis();
  this->live_ = grams;
  // Spool lifted? Only after it was on the scale while tags were latched.
  if (this->slots_[0].present) {
    if (grams > this->reset_threshold_) {
      this->loaded_since_latch_ = true;
      this->empty_since_ = 0;
    } else if (this->loaded_since_latch_) {
      if (this->empty_since_ == 0)
        this->empty_since_ = now == 0 ? 1 : now;
      else if (now - this->empty_since_ > 1500) {
        ESP_LOGI(TAG, "Spool lifted");
        this->clear_();
        return;
      }
    }
  }
  this->samples_.emplace_back(now, grams);
  while (!this->samples_.empty() && now - this->samples_.front().first > this->stable_ms_)
    this->samples_.pop_front();
  bool was_stable = this->stable_;
  this->stable_ = false;
  if (!this->samples_.empty() && now - this->samples_.front().first >= this->stable_ms_ * 3 / 4) {
    float lo = grams, hi = grams;
    double sum = 0;
    for (auto &p : this->samples_) {
      lo = std::min(lo, p.second);
      hi = std::max(hi, p.second);
      sum += p.second;
    }
    this->stable_ = (hi - lo) <= this->stable_band_;
    float mean = sum / this->samples_.size();
    // Capture once both tags are present; re-capture if the stable value moved.
    if (this->stable_ && this->both_tags_() &&
        (std::isnan(this->captured_) || std::fabs(mean - this->captured_) > this->stable_band_)) {
      this->captured_ = mean;
      ESP_LOGI(TAG, "Weight captured: %.1f g", mean);
    }
  }
  if (this->stable_ != was_stable || true)
    this->dirty_ = true;
}

// ------------------------------------------------------------------ Spoolman
void TaggerApp::lookup_tag_(int slot) {
  const int reader = slot;  // callback below indexes slots_ by this
  TagSlot &s = this->slots_[slot];
  if (!s.present)
    return;
  s.looking = true;
  std::string uid = s.uid;
  char rid[40], rname[40];
  // Both physical readers report as ONE Spoolman reader, so a browser pairs
  // with "the Tagger" (Settings > Tag Scanning) rather than half of it.
  snprintf(rid, sizeof(rid), "%s", this->reader_prefix_.c_str());
  snprintf(rname, sizeof(rname), "Tagger");
  this->http_.request("POST", "/api/v1/tag/scan", scan_body(uid, rid, rname, s.format),
                      [this, reader, uid](const HttpResult &r) {
                        TagSlot &s = this->slots_[reader];
                        if (!s.present || s.uid != uid)
                          return;  // tag changed meanwhile
                        s.looking = false;
                        if (!r.ok()) {
                          s.spool_id = -1;
                          this->online_ = false;
                          this->online_known_ = true;
                          this->set_status_(r.status < 0 ? "Spoolman unreachable" :
                                                "Lookup failed: " + error_detail(r.body), true);
                          ESP_LOGW(TAG, "tag/scan %s failed: %d %s", uid.c_str(), r.status, r.error.c_str());
                        } else {
                          this->online_ = this->online_known_ = true;
                          s.spool_id = parse_scan_spool_id(r.body);
                          ESP_LOGI(TAG, "Tag %d (%s) -> spool %d", reader + 1, uid.c_str(), s.spool_id);
                        }
                        this->resolve_();
                      });
  this->dirty_ = true;
}

void TaggerApp::resolve_() {
  int a = this->slots_[0].present ? this->slots_[0].spool_id : 0;
  int b = this->slots_[1].present ? this->slots_[1].spool_id : 0;
  this->mismatch_ = a > 0 && b > 0 && a != b;
  int id = this->mismatch_ ? 0 : std::max(a, b);
  if (id < 0)
    id = 0;
  if (id != this->spool_id_) {
    this->spool_id_ = id;
    this->spool_ = Spool{};
    if (id > 0)
      this->fetch_spool_(id);
  }
  if (this->mismatch_)
    this->set_status_("Tags belong to different spools (#" + std::to_string(a) + " and #" + std::to_string(b) + ")",
                      true);
  this->dirty_ = true;
}

void TaggerApp::fetch_spool_(int id) {
  this->fetching_spool_ = true;
  this->http_.request("GET", "/api/v1/spool/" + std::to_string(id), "", [this, id](const HttpResult &r) {
    if (id != this->spool_id_)
      return;
    this->fetching_spool_ = false;
    if (r.ok() && parse_spool(r.body, this->spool_)) {
      ESP_LOGI(TAG, "Spool #%d: %s %s, %.0f g left, empty %.0f g", id, this->spool_.title().c_str(),
               this->spool_.name.c_str(), this->spool_.remaining, this->spool_.empty_weight);
    } else {
      this->set_status_("Could not load spool #" + std::to_string(id), true);
    }
    this->dirty_ = true;
  });
}

bool TaggerApp::can_send_() const {
  return this->both_tags_() && this->spool_.valid() && !this->mismatch_ && !std::isnan(this->captured_) &&
         !this->sending_;
}

void TaggerApp::send_weight_() {
  if (!this->can_send_())
    return;
  this->sending_ = true;
  float g = this->captured_;
  int id = this->spool_.id;
  char body[48];
  snprintf(body, sizeof(body), "{\"weight\": %.1f}", g);
  this->set_status_("Sending...");
  this->http_.request("PUT", "/api/v1/spool/" + std::to_string(id) + "/measure", body,
                      [this, id, g](const HttpResult &r) {
                        this->sending_ = false;
                        if (r.ok()) {
                          Spool s;
                          if (parse_spool(r.body, s) && s.id == this->spool_.id)
                            this->spool_ = s;
                          char msg[64];
                          snprintf(msg, sizeof(msg), "Saved %.0f g: %.0f g left", g, this->spool_.remaining);
                          this->set_status_(msg);
                          ESP_LOGI(TAG, "Spool #%d measured at %.1f g -> %.1f g left", id, g, this->spool_.remaining);
                        } else {
                          this->set_status_("Save failed: " + (r.status < 0 ? std::string("unreachable") :
                                                                   error_detail(r.body)), true);
                          ESP_LOGW(TAG, "measure #%d failed: %d %s", id, r.status, r.body.c_str());
                        }
                        this->dirty_ = true;
                      });
}

int TaggerApp::unlinked_reader_() const {
  if (!this->spool_.valid())
    return -1;
  for (int i = 0; i < 2; i++)
    if (this->slots_[i].present && this->slots_[i].spool_id == 0)
      return i;
  return -1;
}

void TaggerApp::link_missing_tag_() {
  int reader = this->unlinked_reader_();
  if (reader < 0)
    return;
  std::string uid = this->slots_[reader].uid;
  int id = this->spool_.id;
  std::string body = json::build_json([&](JsonObject root) {
    root["uid"] = uid;
    if (!this->slots_[reader].format.empty())
      root["format"] = this->slots_[reader].format;
  });
  this->set_status_("Linking tag...");
  this->http_.request("POST", "/api/v1/spool/" + std::to_string(id) + "/tag", body,
                      [this, reader, uid, id](const HttpResult &r) {
                        if (r.ok()) {
                          if (this->slots_[reader].uid == uid)
                            this->slots_[reader].spool_id = id;
                          this->set_status_("Tag " + uid + " linked to #" + std::to_string(id));
                          this->fetch_spool_(id);
                          this->report_scans_();
                        } else {
                          this->set_status_("Link failed: " + error_detail(r.body), true);
                        }
                        this->dirty_ = true;
                      });
}

void TaggerApp::check_health_() {
  this->http_.request("GET", "/api/v1/health", "", [this](const HttpResult &r) {
    bool was = this->online_;
    this->online_ = r.ok();
    this->online_known_ = true;
    if (this->online_ != was)
      ESP_LOGI(TAG, "Spoolman %s", this->online_ ? "online" : "offline");
    this->dirty_ = true;
  });
}

void TaggerApp::reset_capture_() {
  this->captured_ = NAN;
  this->dirty_ = true;
}

void TaggerApp::set_status_(const std::string &msg, bool error) {
  this->status_ = msg;
  this->status_error_ = error;
  this->dirty_ = true;
}

// ------------------------------------------------------------------ UI
static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y, int w = 0) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_pos(l, x, y);
  if (w > 0) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  }
  lv_label_set_text(l, "");
  return l;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 10, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, uint32_t color, int x, int y, int w, int h,
                             lv_event_cb_t cb, void *user) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_IDLE), LV_STATE_DISABLED);
  lv_obj_set_style_text_color(b, lv_color_hex(0x7A8792), LV_STATE_DISABLED);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

void TaggerApp::build_ui() {
  if (this->ui_built_)
    return;
  lv_obj_t *scr = lv_screen_active();
  this->scr_main_ = scr;
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = make_label(scr, &lv_font_montserrat_20, C_DIM, 12, 8);
  lv_label_set_text(title, "Tagger");
  this->btn_clear_ = make_button(scr, LV_SYMBOL_REFRESH " Clear", C_IDLE, 110, 4, 110, 32, &TaggerApp::on_clear_clicked_,
                                 this);
  lv_obj_set_style_text_font(lv_obj_get_child(this->btn_clear_, 0), &lv_font_montserrat_14, 0);
  this->lbl_conn_ = make_label(scr, &lv_font_montserrat_14, C_DIM, 230, 12, 238);
  lv_obj_set_style_text_align(this->lbl_conn_, LV_TEXT_ALIGN_RIGHT, 0);

  // ---- spool card (left)
  lv_obj_t *card = make_card(scr, 10, 40, 300, 270);
  this->swatch_ = lv_obj_create(card);
  lv_obj_set_pos(this->swatch_, 0, 0);
  lv_obj_set_size(this->swatch_, 22, 190);
  lv_obj_set_style_radius(this->swatch_, 6, 0);
  lv_obj_set_style_border_width(this->swatch_, 0, 0);
  lv_obj_set_style_bg_color(this->swatch_, lv_color_hex(C_IDLE), 0);
  this->lbl_head_ = make_label(card, &lv_font_montserrat_20, C_TEXT, 34, 0, 244);
  this->lbl_name_ = make_label(card, &lv_font_montserrat_16, C_SUB, 34, 28, 244);
  this->lbl_info_ = make_label(card, &lv_font_montserrat_14, C_DIM, 34, 52, 244);
  this->lbl_remaining_ = make_label(card, &lv_font_montserrat_28, C_TEXT, 34, 76, 244);
  this->lbl_status_ = lv_label_create(card);
  lv_obj_set_pos(this->lbl_status_, 34, 116);
  lv_obj_set_size(this->lbl_status_, 244, 72);
  lv_label_set_long_mode(this->lbl_status_, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(this->lbl_status_, &lv_font_montserrat_14, 0);
  lv_label_set_text(this->lbl_status_, "");
  for (int i = 0; i < 2; i++) {
    this->pill_[i] = lv_obj_create(card);
    lv_obj_set_pos(this->pill_[i], i * 142, 200);
    lv_obj_set_size(this->pill_[i], 136, 50);
    lv_obj_set_style_radius(this->pill_[i], 8, 0);
    lv_obj_set_style_border_width(this->pill_[i], 0, 0);
    lv_obj_set_style_pad_all(this->pill_[i], 4, 0);
    lv_obj_remove_flag(this->pill_[i], LV_OBJ_FLAG_SCROLLABLE);
    this->lbl_pill_[i] = lv_label_create(this->pill_[i]);
    lv_obj_set_style_text_font(this->lbl_pill_[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(this->lbl_pill_[i], lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_align(this->lbl_pill_[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(this->lbl_pill_[i]);
  }

  // ---- weight + actions (right)
  lv_obj_t *side = make_card(scr, 320, 40, 150, 270);
  lv_obj_set_style_pad_all(side, 0, 0);
  lv_obj_set_style_bg_opa(side, LV_OPA_TRANSP, 0);
  this->lbl_weight_ = make_label(side, &lv_font_montserrat_28, C_TEXT, 0, 0);
  lv_obj_set_width(this->lbl_weight_, 150);
  lv_obj_set_style_text_align(this->lbl_weight_, LV_TEXT_ALIGN_CENTER, 0);
  this->lbl_stable_ = make_label(side, &lv_font_montserrat_14, C_DIM, 0, 36);
  lv_obj_set_width(this->lbl_stable_, 150);
  lv_obj_set_style_text_align(this->lbl_stable_, LV_TEXT_ALIGN_CENTER, 0);
  this->btn_send_ = make_button(side, "Send\nweight", C_OK, 0, 62, 150, 84, &TaggerApp::on_send_clicked_, this);
  this->lbl_send_ = lv_obj_get_child(this->btn_send_, 0);
  this->btn_link_ = make_button(side, "Link tag", C_WARN, 0, 154, 150, 52, &TaggerApp::on_link_clicked_, this);
  this->lbl_link_ = lv_obj_get_child(this->btn_link_, 0);
  this->btn_tare_ = make_button(side, "Tare", C_BLUE, 0, 214, 150, 52, &TaggerApp::on_tare_clicked_, this);

  this->ui_built_ = true;
  this->dirty_ = true;
  this->render_();
}

void TaggerApp::render_() {
  char buf[96];
  // connection
  if (!this->online_known_)
    lv_label_set_text(this->lbl_conn_, "Spoolman: connecting...");
  else
    lv_label_set_text(this->lbl_conn_, this->online_ ? "Spoolman " LV_SYMBOL_OK : "Spoolman offline " LV_SYMBOL_WARNING);
  lv_obj_set_style_text_color(this->lbl_conn_, lv_color_hex(this->online_ || !this->online_known_ ? C_DIM : C_ERR),
                              0);

  // tag pills
  int present = 0;
  for (int i = 0; i < 2; i++) {
    const TagSlot &s = this->slots_[i];
    uint32_t color = C_IDLE;
    if (!s.present) {
      snprintf(buf, sizeof(buf), "Tag %d\nwaiting", i + 1);
    } else {
      present++;
      const char *state = s.looking ? "..." : s.spool_id > 0 ? LV_SYMBOL_OK : s.spool_id == 0 ? "unlinked" : "?";
      snprintf(buf, sizeof(buf), "Tag %d %s (R%d)\n%s", i + 1, state, s.reader + 1, s.uid.c_str());
      color = s.spool_id > 0 ? C_OK : s.spool_id == 0 ? C_WARN : C_IDLE;
    }
    lv_label_set_text(this->lbl_pill_[i], buf);
    lv_obj_set_style_bg_color(this->pill_[i], lv_color_hex(color), 0);
  }

  // spool card
  uint32_t swatch = C_IDLE;
  if (this->spool_.valid()) {
    lv_label_set_text(this->lbl_head_, this->spool_.title().c_str());
    lv_label_set_text(this->lbl_name_, this->spool_.name.c_str());
    snprintf(buf, sizeof(buf), "#%d%s%s%s", this->spool_.id, this->spool_.location.empty() ? "" : "  " LV_SYMBOL_HOME " ",
             this->spool_.location.c_str(), this->spool_.archived ? "  (archived)" : "");
    lv_label_set_text(this->lbl_info_, buf);
    if (std::isnan(this->spool_.remaining))
      lv_label_set_text(this->lbl_remaining_, "");
    else {
      snprintf(buf, sizeof(buf), "%.0f g left", this->spool_.remaining);
      lv_label_set_text(this->lbl_remaining_, buf);
    }
    if (this->spool_.color_hex.size() >= 6)
      swatch = strtoul(this->spool_.color_hex.substr(0, 6).c_str(), nullptr, 16);
  } else {
    const char *head = "Place a spool";
    const char *sub = "Both tags + scale";
    if (present == 1) {
      head = "Waiting for second tag";
      sub = "";
    } else if (present == 2) {
      bool looking = this->slots_[0].looking || this->slots_[1].looking || this->fetching_spool_;
      head = this->mismatch_ ? "Tag mismatch" : looking ? "Looking up..." : "Unknown spool";
      sub = this->mismatch_ || looking ? "" : "New spool, or link to one";
    }
    lv_label_set_text(this->lbl_head_, head);
    lv_label_set_text(this->lbl_name_, sub);
    lv_label_set_text(this->lbl_info_, "");
    lv_label_set_text(this->lbl_remaining_, "");
  }
  lv_obj_set_style_bg_color(this->swatch_, lv_color_hex(swatch), 0);

  // status line
  std::string status = this->status_;
  if (status.empty() && this->spool_.valid() && present == 1)
    status = "Waiting for second tag";
  if (status.empty() && this->spool_.valid() && present == 2 && std::isnan(this->captured_))
    status = "Hold still: capturing weight...";
  if (status.empty() && this->spool_.valid() && !std::isnan(this->spool_.empty_weight) && !std::isnan(this->captured_)) {
    snprintf(buf, sizeof(buf), "Ready: %.0f g on scale = %.0f g filament", this->captured_,
             std::max(0.0f, this->captured_ - this->spool_.empty_weight));
    status = buf;
  }
  lv_label_set_text(this->lbl_status_, status.c_str());
  lv_obj_set_style_text_color(this->lbl_status_, lv_color_hex(this->status_error_ ? 0xFF8A80 : C_SUB), 0);

  // weight
  if (std::isnan(this->live_))
    lv_label_set_text(this->lbl_weight_, "-- g");
  else {
    snprintf(buf, sizeof(buf), "%.0f g", this->live_);
    lv_label_set_text(this->lbl_weight_, buf);
  }
  if (!std::isnan(this->captured_))
    snprintf(buf, sizeof(buf), LV_SYMBOL_OK " captured %.0f g", this->captured_);
  else
    snprintf(buf, sizeof(buf), "%s", this->stable_ ? "stable" : "settling...");
  lv_label_set_text(this->lbl_stable_, buf);
  lv_obj_set_style_text_color(this->lbl_stable_, lv_color_hex(!std::isnan(this->captured_) ? 0x7FD69B : C_DIM), 0);

  // buttons
  bool new_mode = this->both_unknown_();
  if (new_mode)
    lv_label_set_text(this->lbl_send_, this->creating_ ? "Creating..." :
                                           this->new_armed_until_ ? "Tap again\nto create" : LV_SYMBOL_PLUS " New\nspool");
  else
    lv_label_set_text(this->lbl_send_, "Send\nweight");
  lv_obj_set_style_bg_color(this->btn_send_, lv_color_hex(new_mode && this->new_armed_until_ ? C_WARN : C_OK), 0);
  if ((new_mode && !this->creating_ && !this->linking_) || (!new_mode && this->can_send_()))
    lv_obj_remove_state(this->btn_send_, LV_STATE_DISABLED);
  else
    lv_obj_add_state(this->btn_send_, LV_STATE_DISABLED);
  bool link_one = this->unlinked_reader_() >= 0;
  bool link_pick = this->both_unknown_();
  lv_label_set_text(this->lbl_link_, link_pick ? "Link to\nspool..." : "Link tag");
  if ((link_one || link_pick) && !this->linking_)
    lv_obj_remove_flag(this->btn_link_, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(this->btn_link_, LV_OBJ_FLAG_HIDDEN);
}

void TaggerApp::on_send_clicked_(lv_event_t *e) {
  auto *app = static_cast<TaggerApp *>(lv_event_get_user_data(e));
  if (app->both_unknown_())
    app->new_spool_();
  else
    app->send_weight_();
}
void TaggerApp::on_link_clicked_(lv_event_t *e) {
  auto *app = static_cast<TaggerApp *>(lv_event_get_user_data(e));
  if (app->both_unknown_())
    app->open_picker_();
  else
    app->link_missing_tag_();
}
void TaggerApp::on_clear_clicked_(lv_event_t *e) {
  static_cast<TaggerApp *>(lv_event_get_user_data(e))->clear_();
}
void TaggerApp::on_tare_clicked_(lv_event_t *e) {
  auto *app = static_cast<TaggerApp *>(lv_event_get_user_data(e));
  if (app->tare_action_)
    app->tare_action_();
  app->reset_capture_();
  app->samples_.clear();
  app->set_status_("Scale tared");
}

// ------------------------------------------------------------------ new spool
static std::string url_encode(const std::string &s) {
  std::string out;
  char hex[4];
  for (unsigned char ch : s) {
    if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
      out += (char) ch;
    else {
      snprintf(hex, sizeof(hex), "%%%02X", ch);
      out += hex;
    }
  }
  return out;
}

void TaggerApp::new_spool_() {
  if (!this->both_unknown_() || this->creating_)
    return;
  uint32_t now = millis();
  if (this->new_armed_until_ == 0) {  // first tap: arm for 4 s
    this->new_armed_until_ = now + 4000;
    this->dirty_ = true;
    return;
  }
  this->new_armed_until_ = 0;
  this->creating_ = true;
  this->set_status_("Creating new spool...");
  if (this->placeholder_id_ > 0) {
    this->create_spool_with_filament_(this->placeholder_id_);
    return;
  }
  // Find (or create once) the placeholder filament new spools start on.
  this->http_.request(
      "GET", "/api/v1/filament?name=" + url_encode(this->placeholder_name_), "", [this](const HttpResult &r) {
        JsonDocument doc = json::parse_json(r.body);
        if (r.ok() && doc.is<JsonArrayConst>()) {
          for (JsonObjectConst f : doc.as<JsonArrayConst>())
            if (this->placeholder_name_ == (f["name"] | ""))
              this->placeholder_id_ = f["id"] | 0;
        }
        if (this->placeholder_id_ > 0) {
          this->create_spool_with_filament_(this->placeholder_id_);
          return;
        }
        if (!r.ok()) {
          this->creating_ = false;
          this->set_status_("Spoolman unreachable", true);
          return;
        }
        std::string body = json::build_json([this](JsonObject root) {
          root["name"] = this->placeholder_name_;
          root["density"] = 1.24;
          root["diameter"] = 1.75;
          root["comment"] = "Placeholder used by Tagger for newly added spools. Change the spool's filament in "
                            "Spoolman.";
        });
        this->http_.request("POST", "/api/v1/filament", body, [this](const HttpResult &r) {
          JsonDocument d = json::parse_json(r.body);
          if (r.ok() && (d["id"] | 0) > 0) {
            this->placeholder_id_ = d["id"] | 0;
            ESP_LOGI(TAG, "Created placeholder filament #%d '%s'", this->placeholder_id_,
                     this->placeholder_name_.c_str());
            this->create_spool_with_filament_(this->placeholder_id_);
          } else {
            this->creating_ = false;
            this->set_status_("Could not create placeholder filament: " + error_detail(r.body), true);
          }
        });
      });
}

void TaggerApp::create_spool_with_filament_(int filament_id) {
  float g = this->captured_;
  std::string body = json::build_json([&](JsonObject root) {
    root["filament_id"] = filament_id;
    char note[96];
    if (!std::isnan(g))
      snprintf(note, sizeof(note), "Added by Tagger. Gross weight when added: %.0f g.", g);
    else
      snprintf(note, sizeof(note), "Added by Tagger.");
    root["comment"] = note;
  });
  this->http_.request("POST", "/api/v1/spool", body, [this](const HttpResult &r) {
    Spool s;
    if (!r.ok() || !parse_spool(r.body, s)) {
      this->creating_ = false;
      this->set_status_("Create spool failed: " + (r.status < 0 ? std::string("unreachable") : error_detail(r.body)),
                        true);
      return;
    }
    ESP_LOGI(TAG, "Created spool #%d", s.id);
    std::vector<std::string> uids;
    for (auto &t : this->slots_)
      if (t.present && t.spool_id == 0)
        uids.push_back(t.uid);
    this->creating_ = false;
    this->linking_ = true;
    this->set_status_("New spool #" + std::to_string(s.id) + ": fill in details in Spoolman");
    this->link_tags_to_(s.id, uids);
  });
}

void TaggerApp::report_scans_() {
  for (auto &s : this->slots_) {
    if (!s.present)
      continue;
    std::string uid = s.uid;
    this->http_.request("POST", "/api/v1/tag/scan", scan_body(uid, this->reader_prefix_, "Tagger", s.format),
                        [uid](const HttpResult &r) {
                          ESP_LOGD(TAG, "Re-reported %s: %d", uid.c_str(), r.status);
                        });
  }
}

void TaggerApp::refresh_spool_() {
  int id = this->spool_.id;
  this->http_.request("GET", "/api/v1/spool/" + std::to_string(id), "", [this, id](const HttpResult &r) {
    if (id != this->spool_.id || this->sending_)
      return;
    Spool s;
    if (r.ok() && parse_spool(r.body, s)) {
      this->spool_ = s;
      this->dirty_ = true;
    }
  });
}

// ------------------------------------------------------------------ spool picker
bool TaggerApp::both_unknown_() const {
  return this->both_tags_() && this->slots_[0].spool_id == 0 && this->slots_[1].spool_id == 0;
}

void TaggerApp::open_picker_() {
  if (this->scr_pick_ == nullptr) {
    lv_obj_t *s = lv_obj_create(nullptr);
    this->scr_pick_ = s;
    lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    make_button(s, LV_SYMBOL_LEFT " Back", C_IDLE, 6, 4, 100, 36, &TaggerApp::on_pick_back_, this);
    lv_obj_t *t = make_label(s, &lv_font_montserrat_20, C_TEXT, 118, 10, 350);
    lv_label_set_text(t, "Link tags to spool");
    this->pick_box_ = lv_obj_create(s);
    lv_obj_set_pos(this->pick_box_, 6, 46);
    lv_obj_set_size(this->pick_box_, 468, 268);
    lv_obj_set_style_bg_opa(this->pick_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(this->pick_box_, 0, 0);
    lv_obj_set_style_pad_all(this->pick_box_, 2, 0);
    lv_obj_set_style_pad_row(this->pick_box_, 6, 0);
    lv_obj_set_flex_flow(this->pick_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(this->pick_box_, LV_DIR_VER);
    this->pick_status_ = make_label(s, &lv_font_montserrat_16, C_DIM, 20, 150, 440);
    // confirmation bar (hidden until a row is tapped)
    this->pick_confirm_ = make_card(s, 6, 226, 468, 88);
    lv_obj_set_style_bg_color(this->pick_confirm_, lv_color_hex(0x2A3440), 0);
    this->pick_confirm_lbl_ = make_label(this->pick_confirm_, &lv_font_montserrat_16, C_TEXT, 0, 0, 240);
    lv_label_set_long_mode(this->pick_confirm_lbl_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(this->pick_confirm_lbl_, 66);
    make_button(this->pick_confirm_, "Cancel", C_IDLE, 252, 8, 90, 52, &TaggerApp::on_pick_cancel_, this);
    make_button(this->pick_confirm_, "Link", C_OK, 352, 8, 94, 52, &TaggerApp::on_pick_confirm_, this);
  }
  lv_obj_add_flag(this->pick_confirm_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(this->pick_box_);
  lv_obj_remove_flag(this->pick_status_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(this->pick_status_, "Loading spools...");
  lv_screen_load(this->scr_pick_);
  this->http_.request("GET", "/api/v1/spool", "", [this](const HttpResult &r) {
    if (lv_screen_active() != this->scr_pick_)
      return;
    this->pick_spools_.clear();
    JsonDocument doc = json::parse_json(r.body);
    if (!r.ok() || !doc.is<JsonArrayConst>()) {
      lv_label_set_text(this->pick_status_, r.status < 0 ? "Spoolman unreachable" : "Could not load spools");
      return;
    }
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
      Spool s;
      if (parse_spool(o, s) && !s.archived)
        this->pick_spools_.push_back(s);
    }
    std::sort(this->pick_spools_.begin(), this->pick_spools_.end(), [](const Spool &a, const Spool &b) {
      if (a.vendor != b.vendor)
        return a.vendor < b.vendor;
      if (a.material != b.material)
        return a.material < b.material;
      return a.id < b.id;
    });
    this->render_picker_list_();
  });
}

void TaggerApp::render_picker_list_() {
  lv_obj_clean(this->pick_box_);
  if (this->pick_spools_.empty()) {
    lv_label_set_text(this->pick_status_, "No spools in Spoolman");
    return;
  }
  lv_obj_add_flag(this->pick_status_, LV_OBJ_FLAG_HIDDEN);
  char buf[160];
  for (const Spool &s : this->pick_spools_) {
    lv_obj_t *row = lv_button_create(this->pick_box_);
    lv_obj_set_size(row, 456, 58);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_user_data(row, (void *) (intptr_t) s.id);
    lv_obj_add_event_cb(row, &TaggerApp::on_pick_row_, LV_EVENT_CLICKED, this);
    lv_obj_t *sw = lv_obj_create(row);
    lv_obj_set_pos(sw, 0, 0);
    lv_obj_set_size(sw, 18, 46);
    lv_obj_set_style_radius(sw, 4, 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    uint32_t col = s.color_hex.size() >= 6 ? strtoul(s.color_hex.substr(0, 6).c_str(), nullptr, 16) : C_IDLE;
    lv_obj_set_style_bg_color(sw, lv_color_hex(col), 0);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l1 = make_label(row, &lv_font_montserrat_16, C_TEXT, 28, 0, 330);
    snprintf(buf, sizeof(buf), "#%d  %s  %s", s.id, s.title().c_str(), s.name.c_str());
    lv_label_set_text(l1, buf);
    lv_obj_t *l2 = make_label(row, &lv_font_montserrat_14, C_DIM, 28, 24, 330);
    snprintf(buf, sizeof(buf), "%s%s%s", s.location.empty() ? "" : s.location.c_str(), s.tags.empty() ? "" : "   ",
             s.tags.empty() ? "" : (std::to_string(s.tags.size()) + " tag(s) linked").c_str());
    lv_label_set_text(l2, buf);
    lv_obj_t *l3 = make_label(row, &lv_font_montserrat_16, C_SUB, 364, 12, 84);
    lv_obj_set_style_text_align(l3, LV_TEXT_ALIGN_RIGHT, 0);
    if (!std::isnan(s.remaining)) {
      snprintf(buf, sizeof(buf), "%.0f g", s.remaining);
      lv_label_set_text(l3, buf);
    }
  }
}

void TaggerApp::show_pick_confirm_(int id) {
  this->pick_id_ = id;
  const Spool *sp = nullptr;
  for (auto &s : this->pick_spools_)
    if (s.id == id)
      sp = &s;
  char buf[160];
  snprintf(buf, sizeof(buf), "Link both tags to\n#%d %s %s?", id, sp ? sp->title().c_str() : "",
           sp ? sp->name.c_str() : "");
  lv_label_set_text(this->pick_confirm_lbl_, buf);
  lv_obj_remove_flag(this->pick_confirm_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->pick_confirm_);
}

void TaggerApp::close_picker_() {
  if (this->scr_main_ != nullptr)
    lv_screen_load(this->scr_main_);
  this->dirty_ = true;
}

void TaggerApp::link_tags_to_(int id, std::vector<std::string> uids) {
  if (uids.empty()) {
    this->linking_ = false;
    for (auto &s : this->slots_)
      if (s.present && s.spool_id == 0)
        s.spool_id = id;
    this->set_status_("Tags linked to #" + std::to_string(id));
    this->resolve_();
    if (this->spool_id_ == id)
      this->fetch_spool_(id);
    this->report_scans_();
    return;
  }
  std::string uid = uids.back();
  uids.pop_back();
  std::string body = json::build_json([&](JsonObject root) { root["uid"] = uid; });
  this->http_.request("POST", "/api/v1/spool/" + std::to_string(id) + "/tag", body,
                      [this, id, uid, uids](const HttpResult &r) {
                        if (!r.ok()) {
                          this->linking_ = false;
                          this->set_status_("Link " + uid + " failed: " + error_detail(r.body), true);
                          ESP_LOGW(TAG, "Link %s -> #%d failed: %d %s", uid.c_str(), id, r.status, r.body.c_str());
                          return;
                        }
                        ESP_LOGI(TAG, "Linked tag %s to spool #%d", uid.c_str(), id);
                        for (auto &s : this->slots_)
                          if (s.present && s.uid == uid)
                            s.spool_id = id;
                        this->link_tags_to_(id, uids);
                      });
}

void TaggerApp::on_pick_row_(lv_event_t *e) {
  auto *app = static_cast<TaggerApp *>(lv_event_get_user_data(e));
  lv_obj_t *row = lv_event_get_current_target_obj(e);
  app->show_pick_confirm_((int) (intptr_t) lv_obj_get_user_data(row));
}
void TaggerApp::on_pick_cancel_(lv_event_t *e) {
  auto *app = static_cast<TaggerApp *>(lv_event_get_user_data(e));
  lv_obj_add_flag(app->pick_confirm_, LV_OBJ_FLAG_HIDDEN);
}
void TaggerApp::on_pick_back_(lv_event_t *e) { static_cast<TaggerApp *>(lv_event_get_user_data(e))->close_picker_(); }
void TaggerApp::on_pick_confirm_(lv_event_t *e) {
  auto *app = static_cast<TaggerApp *>(lv_event_get_user_data(e));
  std::vector<std::string> uids;
  for (auto &s : app->slots_)
    if (s.present && s.spool_id == 0)
      uids.push_back(s.uid);
  int id = app->pick_id_;
  app->close_picker_();
  if (uids.empty() || id <= 0)
    return;
  app->linking_ = true;
  app->set_status_("Linking " + std::to_string(uids.size()) + " tag(s) to #" + std::to_string(id) + "...");
  ESP_LOGI(TAG, "Linking %u tag(s) to spool #%d", (unsigned) uids.size(), id);
  app->link_tags_to_(id, uids);
}

}  // namespace tagger_app
}  // namespace esphome
