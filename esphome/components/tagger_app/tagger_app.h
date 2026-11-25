// Tagger application: two-tag spool capture, stable-weight capture and the
// Spoolman-backed Scan screen (phase 1 of the touch Spoolman client).
//
// Flow (see TODO.md / agent.md):
//   1. Tags are LATCHED in arrival order, whichever reader sees them: the
//      first UID is held while waiting for a second, different UID. Tags
//      dropping out of a reader's field are ignored (readers flicker, and both
//      readers may see the same tag). Each UID is resolved with POST /tag/scan;
//      either tag identifies the spool.
//   2. With both tags latched the scale reading is captured once stable.
//   3. The identified spool is fetched (GET /spool/{id}) and shown.
//   4. "Send weight" -> PUT /spool/{id}/measure {"weight": captured gross g};
//      Spoolman subtracts the empty-spool weight itself.
//   5. Reset: the spool is lifted (weight was above reset_threshold while
//      latched, then stays below it), or the Clear button is tapped.
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "http_worker.h"
#include "lvgl.h"
#include "spoolman.h"

namespace esphome {
namespace tagger_app {

class TaggerApp : public Component {
 public:
  // ---- configuration (from YAML)
  void set_base_url(const std::string &url) { this->http_.set_base_url(url); }
  void set_reader_prefix(const std::string &p) { this->reader_prefix_ = p; }
  void set_stable_band(float g) { this->stable_band_ = g; }
  void set_stable_ms(uint32_t ms) { this->stable_ms_ = ms; }
  void set_reset_threshold(float g) { this->reset_threshold_ = g; }
  void set_placeholder_filament(const std::string &n) { this->placeholder_name_ = n; }
  void set_tare_action(std::function<void()> f) { this->tare_action_ = std::move(f); }

  // ---- inputs (called from YAML lambdas)
  void on_tag(int reader, const std::string &uid, const std::string &format);
  void on_tag_removed(int reader);
  void on_weight(float grams);
  void set_battery(bool present, int percent, int millivolts, bool usb, bool charging);
  void build_ui();  // call from lvgl on_boot

  // ---- Component
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  struct TagSlot {  // slots_[0] = first tag latched, slots_[1] = second
    bool present{false};
    int reader{-1};      // physical reader (0/1) that first saw this tag
    std::string uid;     // normalized
    std::string format;
    int spool_id{-1};    // -1 = lookup pending/failed, 0 = not linked, >0 = spool
    bool looking{false};
  };

  // logic
  void lookup_tag_(int reader);
  void resolve_();
  void fetch_spool_(int id);
  void send_weight_();
  void link_missing_tag_();
  void check_health_();
  void reset_capture_();
  void clear_();  // drop latched tags and start over
  bool both_unknown_() const;
  // spool picker: link the latched (unlinked) tags to an existing spool
  void open_picker_();
  void close_picker_();
  void render_picker_list_();
  void show_pick_confirm_(int id);
  void link_tags_to_(int id, std::vector<std::string> uids);
  // new spool: placeholder filament -> POST /spool -> link tags -> re-report scans
  void new_spool_();
  void create_spool_with_filament_(int filament_id);
  void report_scans_();   // re-POST /tag/scan for latched tags (browser auto-navigate)
  void refresh_spool_();  // periodic GET /spool/{id} so web edits show up
  bool both_tags_() const { return this->slots_[0].present && this->slots_[1].present; }
  int unlinked_reader_() const;
  bool can_send_() const;

  // UI
  void render_();
  void set_status_(const std::string &msg, bool error = false);
  static void on_send_clicked_(lv_event_t *e);
  static void on_link_clicked_(lv_event_t *e);
  static void on_tare_clicked_(lv_event_t *e);
  static void on_clear_clicked_(lv_event_t *e);
  static void on_pick_row_(lv_event_t *e);
  static void on_pick_cancel_(lv_event_t *e);
  static void on_pick_confirm_(lv_event_t *e);
  static void on_pick_back_(lv_event_t *e);

  HttpWorker http_;
  std::string placeholder_name_{"Unassigned (set filament)"};
  int placeholder_id_{0};
  bool creating_{false};
  uint32_t new_armed_until_{0};  // two-tap confirm for "New spool"
  uint32_t last_refresh_{0};
  std::string reader_prefix_{"tagger"};
  std::function<void()> tare_action_;

  TagSlot slots_[2];
  int spool_id_{0};        // resolved spool, 0 = none
  bool mismatch_{false};
  Spool spool_;
  bool fetching_spool_{false};
  bool sending_{false};
  bool online_{false};
  bool online_known_{false};
  uint32_t last_health_{0};

  // weight
  float live_{NAN};
  float captured_{NAN};
  bool stable_{false};
  float stable_band_{2.0f};
  uint32_t stable_ms_{2000};
  std::deque<std::pair<uint32_t, float>> samples_;
  float reset_threshold_{30.0f};
  bool loaded_since_latch_{false};  // weight exceeded reset_threshold while tags latched
  uint32_t empty_since_{0};         // millis when weight fell below threshold (0 = not empty)

  // battery (AXP2101)
  bool bat_known_{false}, bat_present_{false}, bat_usb_{false}, bat_charging_{false};
  int bat_pct_{0}, bat_mv_{0};

  std::string status_;
  bool status_error_{false};
  bool dirty_{true};

  // widgets
  bool ui_built_{false};
  lv_obj_t *lbl_conn_{nullptr};
  lv_obj_t *lbl_bat_{nullptr};
  lv_obj_t *swatch_{nullptr};
  lv_obj_t *lbl_head_{nullptr};
  lv_obj_t *lbl_name_{nullptr};
  lv_obj_t *lbl_info_{nullptr};
  lv_obj_t *lbl_remaining_{nullptr};
  lv_obj_t *lbl_status_{nullptr};
  lv_obj_t *pill_[2]{nullptr, nullptr};
  lv_obj_t *lbl_pill_[2]{nullptr, nullptr};
  lv_obj_t *lbl_weight_{nullptr};
  lv_obj_t *lbl_stable_{nullptr};
  lv_obj_t *btn_send_{nullptr};
  lv_obj_t *btn_link_{nullptr};
  lv_obj_t *btn_tare_{nullptr};
  lv_obj_t *btn_clear_{nullptr};
  lv_obj_t *lbl_link_{nullptr};
  lv_obj_t *lbl_send_{nullptr};
  lv_obj_t *scr_main_{nullptr};
  // picker screen
  std::vector<Spool> pick_spools_;
  int pick_id_{0};
  bool linking_{false};
  lv_obj_t *scr_pick_{nullptr};
  lv_obj_t *pick_box_{nullptr};
  lv_obj_t *pick_status_{nullptr};
  lv_obj_t *pick_confirm_{nullptr};
  lv_obj_t *pick_confirm_lbl_{nullptr};
};

}  // namespace tagger_app
}  // namespace esphome
