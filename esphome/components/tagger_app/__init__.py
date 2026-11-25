"""Tagger touch client for Spoolman (local ESPHome component)."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

DEPENDENCIES = ["lvgl", "network"]
AUTO_LOAD = ["json"]

CONF_SPOOLMAN_URL = "spoolman_url"
CONF_READER_PREFIX = "reader_prefix"
CONF_STABLE_BAND = "stable_band"
CONF_STABLE_TIME = "stable_time"
CONF_RESET_THRESHOLD = "reset_threshold"

tagger_app_ns = cg.esphome_ns.namespace("tagger_app")
TaggerApp = tagger_app_ns.class_("TaggerApp", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TaggerApp),
        cv.Required(CONF_SPOOLMAN_URL): cv.string_strict,
        cv.Optional(CONF_READER_PREFIX, default="tagger"): cv.string_strict,
        cv.Optional(CONF_STABLE_BAND, default=2.0): cv.positive_float,
        cv.Optional(CONF_STABLE_TIME, default="2s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_RESET_THRESHOLD, default=30.0): cv.positive_float,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_base_url(config[CONF_SPOOLMAN_URL]))
    cg.add(var.set_reader_prefix(config[CONF_READER_PREFIX]))
    cg.add(var.set_stable_band(config[CONF_STABLE_BAND]))
    cg.add(var.set_stable_ms(config[CONF_STABLE_TIME].total_milliseconds))
    cg.add(var.set_reset_threshold(config[CONF_RESET_THRESHOLD]))
