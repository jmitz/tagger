// Tag parsing, Spoolman lookup and on-screen rendering shared by both readers.
//
// Identification order for a tag on a reader:
//   1. Spoolman: a spool whose extra field (key set by spoolman_uid_field,
//      default "nfc_uid") holds this tag's UID
//      (Spoolman stores extra-field values JSON-encoded, e.g. "\"A140FEA2\"").
//   2. OpenSpool NDEF on the tag: single "application/json" record, e.g.
//      {"protocol":"openspool","version":"1.0","type":"PLA",
//       "color_hex":"FFAABB","brand":"Generic","min_temp":"220","max_temp":"240"}
//      https://github.com/spuder/OpenSpool/blob/main/docs/rfid.md
//   3. Otherwise just the UID.
//
// Weight: on "Send weight", remaining_weight = scale reading - empty-spool
// weight (spool.spool_weight, else filament.spool_weight) is PATCHed to Spoolman.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include "esphome/components/json/json_util.h"
#include "esphome/components/nfc/nfc_tag.h"
#include "esphome/core/log.h"
#include "lvgl.h"

namespace tagger {

static const char *const TAG = "tagger";

enum class TagState { NONE, OPENSPOOL, OTHER_NDEF, NO_NDEF };
enum class SmState { IDLE, LOOKING_UP, FOUND, NOT_LINKED, ERROR };

struct SpoolInfo {
  TagState state = TagState::NONE;
  std::string uid;
  // OpenSpool NDEF content
  std::string type, brand, color_hex, min_temp, max_temp, raw;
  // Spoolman match
  SmState sm = SmState::IDLE;
  int sm_id = 0;
  std::string sm_vendor, sm_name, sm_material, sm_color;
  float sm_remaining = NAN;    // g of filament left, per Spoolman
  float sm_empty = NAN;        // empty spool weight, g
  std::string status;          // last Spoolman message shown on the card
};

inline SpoolInfo readers[2];

// ------------------------------------------------------------------ tags
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
  info.sm = SmState::LOOKING_UP;
  ESP_LOGI(TAG, "Reader %d: UID %s state %d type '%s' brand '%s'", idx + 1, info.uid.c_str(),
           (int) info.state, info.type.c_str(), info.brand.c_str());
  readers[idx] = info;
}

inline void clear_tag(int idx) {
  ESP_LOGI(TAG, "Reader %d: tag removed", idx + 1);
  readers[idx] = SpoolInfo{};
}

inline bool has_tag(int idx) { return readers[idx].state != TagState::NONE; }

// ------------------------------------------------------------------ Spoolman
// Base URL from the spoolman_url secret: adds "http://" if no scheme was
// given and drops a trailing slash, so "spoolman.local:7912" also works.
inline std::string base_url(const char *configured) {
  std::string u = configured;
  if (u.find("://") == std::string::npos) u = "http://" + u;
  while (!u.empty() && u.back() == '/') u.pop_back();
  return u;
}

// Canonical UID form: upper-case hex only, so "a1-40-fe-a2", "A1:40:FE:A2"
// and "A140FEA2" all match. Also strips the quotes Spoolman stores extra
// field values with (they are JSON-encoded, e.g. "\"A140FEA2\"").
inline std::string norm_uid(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return !std::isxdigit(c); }), s.end());
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

inline std::string extra_value(JsonVariantConst v) { return norm_uid(v | ""); }

// Handles GET /api/v1/spool for the tag currently on reader idx.
inline void on_spool_list(int idx, int status, const std::string &body, const char *uid_field) {
  SpoolInfo &s = readers[idx];
  if (s.state == TagState::NONE) return;  // tag removed while the request ran
  if (status != 200) {
    s.sm = SmState::ERROR;
    s.status = "Spoolman HTTP " + std::to_string(status);
    ESP_LOGW(TAG, "Reader %d: Spoolman lookup failed, HTTP %d", idx + 1, status);
    return;
  }
  JsonDocument doc = esphome::json::parse_json(body);
  if (doc.isNull() || !doc.is<JsonArrayConst>()) {
    s.sm = SmState::ERROR;
    s.status = "Spoolman: bad response";
    ESP_LOGW(TAG, "Reader %d: Spoolman response not a JSON array (%u bytes)", idx + 1, (unsigned) body.size());
    return;
  }
  const std::string want = norm_uid(s.uid);
  for (JsonObjectConst spool : doc.as<JsonArrayConst>()) {
    if (extra_value(spool["extra"][uid_field]) != want) continue;
    JsonObjectConst fil = spool["filament"];
    s.sm = SmState::FOUND;
    s.sm_id = spool["id"] | 0;
    s.sm_vendor = fil["vendor"]["name"] | "";
    s.sm_name = fil["name"] | "";
    s.sm_material = fil["material"] | "";
    s.sm_color = fil["color_hex"] | "";
    s.sm_remaining = spool["remaining_weight"] | NAN;
    s.sm_empty = spool["spool_weight"] | (fil["spool_weight"] | NAN);
    s.status = "";
    ESP_LOGI(TAG, "Reader %d: UID %s -> Spoolman spool #%d (%s %s %s), %.0f g left, empty %.0f g", idx + 1,
             s.uid.c_str(), s.sm_id, s.sm_vendor.c_str(), s.sm_material.c_str(), s.sm_name.c_str(),
             s.sm_remaining, s.sm_empty);
    return;
  }
  s.sm = SmState::NOT_LINKED;
  s.status = "Not in Spoolman";
  ESP_LOGI(TAG, "Reader %d: UID %s not linked to any Spoolman spool (%u spools checked)", idx + 1, s.uid.c_str(),
           (unsigned) doc.as<JsonArrayConst>().size());
}

inline void on_http_error(int idx, const char *what) {
  SpoolInfo &s = readers[idx];
  if (s.state == TagState::NONE) return;
  if (s.sm == SmState::LOOKING_UP) s.sm = SmState::ERROR;
  s.status = std::string("Spoolman unreachable (") + what + ")";
  ESP_LOGW(TAG, "Reader %d: Spoolman %s request failed", idx + 1, what);
}

// Validates a weight send; on failure sets the card message and returns false.
inline bool can_send_weight(int idx, float grams) {
  SpoolInfo &s = readers[idx];
  if (s.state == TagState::NONE) return false;
  if (s.sm != SmState::FOUND) {
    s.status = s.sm == SmState::NOT_LINKED ? "Link this tag in Spoolman first" : "Spool not identified yet";
    return false;
  }
  if (std::isnan(grams)) {
    s.status = "No scale reading";
    return false;
  }
  if (std::isnan(s.sm_empty)) {
    s.status = "Set empty spool weight in Spoolman";
    return false;
  }
  if (grams - s.sm_empty < -5) {
    s.status = "Scale below empty-spool weight";
    return false;
  }
  s.status = "Sending...";
  return true;
}

inline float remaining_from_scale(int idx, float grams) {
  return std::max(0.0f, grams - readers[idx].sm_empty);
}

inline std::string weight_body(int idx, float grams) {
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"remaining_weight\": %.1f}", remaining_from_scale(idx, grams));
  return buf;
}

inline void on_weight_sent(int idx, int status, float grams) {
  SpoolInfo &s = readers[idx];
  if (status >= 200 && status < 300) {
    s.sm_remaining = remaining_from_scale(idx, grams);
    char buf[48];
    snprintf(buf, sizeof(buf), "Saved: %.0f g left", s.sm_remaining);
    s.status = buf;
    ESP_LOGI(TAG, "Reader %d: spool #%d remaining_weight -> %.1f g", idx + 1, s.sm_id, s.sm_remaining);
  } else {
    s.status = "Save failed: HTTP " + std::to_string(status);
    ESP_LOGW(TAG, "Reader %d: PATCH spool #%d failed, HTTP %d", idx + 1, s.sm_id, status);
  }
}

// ------------------------------------------------------------------ UI
inline uint32_t parse_color(const std::string &hex) {
  std::string h = hex;
  if (!h.empty() && h[0] == '#') h.erase(0, 1);
  if (h.size() < 6) return 0x404040;
  return strtoul(h.substr(0, 6).c_str(), nullptr, 16);
}

// Updates one reader card: headline, detail text, and colour swatch.
inline void render(int idx, lv_obj_t *headline, lv_obj_t *detail, lv_obj_t *swatch) {
  const SpoolInfo &s = readers[idx];
  std::string head, body;
  uint32_t color = 0x303030;
  char buf[64];

  if (s.state == TagState::NONE) {
    head = "No spool";
    body = "Place a tagged spool";
  } else if (s.sm == SmState::FOUND) {
    head = (s.sm_vendor.empty() ? "" : s.sm_vendor + " ") + s.sm_material;
    snprintf(buf, sizeof(buf), "#%d %s\n", s.sm_id, s.sm_name.c_str());
    body = buf;
    if (!std::isnan(s.sm_remaining)) {
      snprintf(buf, sizeof(buf), "%.0f g left", s.sm_remaining);
      body += buf;
    }
    color = parse_color(s.sm_color);
  } else if (s.state == TagState::OPENSPOOL) {
    head = (s.brand.empty() ? std::string("Generic") : s.brand) + " " + s.type;
    body = s.min_temp + "-" + s.max_temp + " C  UID " + norm_uid(s.uid);
    color = parse_color(s.color_hex);
  } else {
    head = s.state == TagState::OTHER_NDEF ? "Unknown tag" : "Unlinked tag";
    body = "UID " + norm_uid(s.uid);
  }
  if (s.state != TagState::NONE) {
    if (s.sm == SmState::LOOKING_UP) body += "\nLooking up...";
    else if (!s.status.empty()) body += "\n" + s.status;
  }
  lv_label_set_text(headline, head.c_str());
  lv_label_set_text(detail, body.c_str());
  lv_obj_set_style_bg_color(swatch, lv_color_hex(color), LV_PART_MAIN);
}

}  // namespace tagger
