#include "spoolman.h"

#include <algorithm>
#include <cctype>

namespace esphome {
namespace tagger_app {

std::string norm_uid(const std::string &uid) {
  std::string s;
  s.reserve(uid.size());
  for (unsigned char c : uid)
    if (std::isxdigit(c))
      s += (char) std::toupper(c);
  return s;
}

bool Spool::has_tag(const std::string &uid) const {
  std::string u = norm_uid(uid);
  return std::find(this->tags.begin(), this->tags.end(), u) != this->tags.end();
}

std::string Spool::title() const {
  if (this->vendor.empty())
    return this->material;
  return this->vendor + " " + this->material;
}

static float num(JsonVariantConst v) { return v.is<float>() || v.is<int>() ? v.as<float>() : NAN; }

bool parse_spool(JsonObjectConst o, Spool &s) {
  if (o.isNull() || !o["id"].is<int>())
    return false;
  s = Spool{};
  s.id = o["id"].as<int>();
  JsonObjectConst f = o["filament"];
  s.vendor = f["vendor"]["name"] | "";
  s.name = f["name"] | "";
  s.material = f["material"] | "";
  s.color_hex = f["color_hex"] | "";
  s.location = o["location"] | "";
  s.remaining = num(o["remaining_weight"]);
  s.initial = num(o["initial_weight"]);
  if (std::isnan(s.initial))
    s.initial = num(f["weight"]);
  s.empty_weight = num(o["spool_weight"]);
  if (std::isnan(s.empty_weight))
    s.empty_weight = num(f["spool_weight"]);
  s.archived = o["archived"] | false;
  for (JsonObjectConst t : o["tags"].as<JsonArrayConst>())
    s.tags.push_back(norm_uid(t["uid"] | ""));
  return true;
}

bool parse_spool(const std::string &json, Spool &out) {
  JsonDocument doc = json::parse_json(json);
  return parse_spool(doc.as<JsonObjectConst>(), out);
}

int parse_scan_spool_id(const std::string &json) {
  JsonDocument doc = json::parse_json(json);
  if (doc.isNull())
    return 0;
  return doc["matched_spool_id"] | 0;
}

std::string scan_body(const std::string &uid, const std::string &reader_id, const std::string &reader_name,
                      const std::string &format) {
  return json::build_json([&](JsonObject root) {
    root["uid"] = norm_uid(uid);
    root["reader_id"] = reader_id;
    root["name"] = reader_name;
    if (!format.empty())
      root["format"] = format;
  });
}

std::string error_detail(const std::string &body) {
  JsonDocument doc = json::parse_json(body);
  if (doc.isNull())
    return body.substr(0, 60);
  if (doc["message"].is<const char *>())
    return doc["message"].as<std::string>();
  if (doc["detail"].is<const char *>())
    return doc["detail"].as<std::string>();
  if (doc["detail"][0]["msg"].is<const char *>())
    return doc["detail"][0]["msg"].as<std::string>();
  return body.substr(0, 60);
}

}  // namespace tagger_app
}  // namespace esphome
