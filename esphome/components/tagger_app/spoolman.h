// Spoolman data model (subset used by the device) and JSON parsing.
// API reference: GET {spoolman}/api/v1/openapi.json (Spoolman 0.27).
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "esphome/components/json/json_util.h"

namespace esphome {
namespace tagger_app {

// Canonical tag UID: upper-case hex, no separators (Spoolman's own form).
std::string norm_uid(const std::string &uid);

struct Spool {
  int id{0};
  std::string vendor;       // filament.vendor.name
  std::string name;         // filament.name
  std::string material;     // filament.material
  std::string color_hex;    // filament.color_hex (RRGGBB), may be empty
  std::string location;
  float remaining{NAN};     // g of filament left
  float initial{NAN};       // g of filament when full
  float empty_weight{NAN};  // spool.spool_weight, else filament.spool_weight
  bool archived{false};
  std::vector<std::string> tags;  // normalized UIDs linked to this spool

  bool valid() const { return this->id > 0; }
  bool has_tag(const std::string &uid) const;
  std::string title() const;  // "Vendor Material"
};

bool parse_spool(const std::string &json, Spool &out);
bool parse_spool(JsonObjectConst obj, Spool &out);

// POST /api/v1/tag/scan response: returns the matched spool id (0 = none).
int parse_scan_spool_id(const std::string &json);

// Builds the JSON body for POST /api/v1/tag/scan.
std::string scan_body(const std::string &uid, const std::string &reader_id, const std::string &reader_name,
                      const std::string &format);

std::string error_detail(const std::string &body);  // FastAPI {"detail": ...} or {"message": ...}

}  // namespace tagger_app
}  // namespace esphome
