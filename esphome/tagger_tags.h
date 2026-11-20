// OpenSpool tag parsing and on-screen rendering shared by both readers.
//
// An OpenSpool tag is an NTAG215/216 with a single NDEF record of type
// "application/json", e.g.
//   {"protocol":"openspool","version":"1.0","type":"PLA",
//    "color_hex":"FFAABB","brand":"Generic","min_temp":"220","max_temp":"240"}
// See https://github.com/spuder/OpenSpool/blob/main/docs/rfid.md
#pragma once

#include <string>
#include "esphome/components/json/json_util.h"
#include "esphome/components/nfc/nfc_tag.h"
#include "esphome/core/log.h"
#include "lvgl.h"

namespace tagger {

static const char *const TAG = "tagger";

enum class TagState { NONE, OPENSPOOL, OTHER_NDEF, NO_NDEF };

struct SpoolInfo {
  TagState state = TagState::NONE;
  std::string uid;
  std::string type;       // material, e.g. PLA
  std::string brand;
  std::string color_hex;  // RRGGBB
  std::string min_temp, max_temp;
  std::string raw;        // payload of the first NDEF record
};

inline SpoolInfo readers[2];

inline void parse_tag(int idx, const std::string &uid, const esphome::nfc::NfcTag &tag) {
  SpoolInfo info;
  info.uid = uid;
  info.state = TagState::NO_NDEF;
  if (tag.has_ndef_message()) {
    auto &records = tag.get_ndef_message()->get_records();
    info.state = TagState::OTHER_NDEF;
    if (!records.empty()) {
      info.raw = records[0]->get_payload();
      if (records[0]->get_type() == "application/json") {
        JsonDocument doc = esphome::json::parse_json(info.raw);
        if (!doc.isNull() && doc["protocol"] == "openspool") {
          info.state = TagState::OPENSPOOL;
          info.type = doc["type"] | "";
          info.brand = doc["brand"] | "";
          info.color_hex = doc["color_hex"] | "";
          info.min_temp = doc["min_temp"] | "";
          info.max_temp = doc["max_temp"] | "";
        }
      }
    }
  }
  ESP_LOGI(TAG, "Reader %d: UID %s state %d type '%s' brand '%s' color %s temp %s-%s", idx + 1,
           info.uid.c_str(), (int) info.state, info.type.c_str(), info.brand.c_str(),
           info.color_hex.c_str(), info.min_temp.c_str(), info.max_temp.c_str());
  readers[idx] = info;
}

inline void clear_tag(int idx) {
  ESP_LOGI(TAG, "Reader %d: tag removed", idx + 1);
  readers[idx] = SpoolInfo{};
}

inline uint32_t color_of(const SpoolInfo &s) {
  if (s.color_hex.size() < 6) return 0x404040;
  return strtoul(s.color_hex.substr(0, 6).c_str(), nullptr, 16);
}

// Updates one reader card: headline, detail text, and colour swatch.
inline void render(int idx, lv_obj_t *headline, lv_obj_t *detail, lv_obj_t *swatch) {
  const SpoolInfo &s = readers[idx];
  std::string head, body;
  uint32_t color = 0x303030;
  switch (s.state) {
    case TagState::NONE:
      head = "No spool";
      body = "Place a tagged spool";
      break;
    case TagState::OPENSPOOL:
      head = (s.brand.empty() ? std::string("Generic") : s.brand) + " " + s.type;
      body = "#" + s.color_hex + "   " + s.min_temp + "-" + s.max_temp + " C\nUID " + s.uid;
      color = color_of(s);
      break;
    case TagState::OTHER_NDEF:
      head = "Not an OpenSpool tag";
      body = "UID " + s.uid;
      break;
    case TagState::NO_NDEF:
      head = "Blank / unformatted tag";
      body = "UID " + s.uid;
      break;
  }
  lv_label_set_text(headline, head.c_str());
  lv_label_set_text(detail, body.c_str());
  lv_obj_set_style_bg_color(swatch, lv_color_hex(color), LV_PART_MAIN);
}

}  // namespace tagger
