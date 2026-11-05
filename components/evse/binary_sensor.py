import logging

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_HEAT,
    DEVICE_CLASS_PLUG,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
import esphome.final_validate as fv

from . import CONF_EVSE_ID, CONF_RCM, CONF_TEMPERATURE_SENSORS, Evse, evse_ns

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["evse"]

EvseBinarySensorType = evse_ns.enum("EvseBinarySensorType")

_FAULT = (DEVICE_CLASS_PROBLEM, ENTITY_CATEGORY_DIAGNOSTIC)

# key: (type, device_class, entity_category, icon)
BINARY_SENSORS = {
    # error codes of esp32-evse
    "pilot_fault": (
        EvseBinarySensorType.BINARY_SENSOR_PILOT_FAULT,
        *_FAULT,
        cv.UNDEFINED,
    ),
    "diode_short": (
        EvseBinarySensorType.BINARY_SENSOR_DIODE_SHORT,
        *_FAULT,
        cv.UNDEFINED,
    ),
    "rcm_triggered": (
        EvseBinarySensorType.BINARY_SENSOR_RCM_TRIGGERED,
        *_FAULT,
        cv.UNDEFINED,
    ),
    "rcm_selftest_fault": (
        EvseBinarySensorType.BINARY_SENSOR_RCM_SELFTEST_FAULT,
        *_FAULT,
        cv.UNDEFINED,
    ),
    "temperature_high": (
        EvseBinarySensorType.BINARY_SENSOR_TEMPERATURE_HIGH,
        DEVICE_CLASS_HEAT,
        ENTITY_CATEGORY_DIAGNOSTIC,
        cv.UNDEFINED,
    ),
    "temperature_fault": (
        EvseBinarySensorType.BINARY_SENSOR_TEMPERATURE_FAULT,
        *_FAULT,
        cv.UNDEFINED,
    ),
    # any error (state E)
    "error": (
        EvseBinarySensorType.BINARY_SENSOR_ERROR,
        DEVICE_CLASS_PROBLEM,
        cv.UNDEFINED,
        cv.UNDEFINED,
    ),
    # consumption, charging time or under power limit reached
    "limit_reached": (
        EvseBinarySensorType.BINARY_SENSOR_LIMIT_REACHED,
        cv.UNDEFINED,
        cv.UNDEFINED,
        "mdi:car-speed-limiter",
    ),
    "pending_authorization": (
        EvseBinarySensorType.BINARY_SENSOR_PENDING_AUTHORIZATION,
        cv.UNDEFINED,
        cv.UNDEFINED,
        "mdi:account-key",
    ),
    # session in progress (states B1 .. D2)
    "vehicle_connected": (
        EvseBinarySensorType.BINARY_SENSOR_VEHICLE_CONNECTED,
        DEVICE_CLASS_PLUG,
        cv.UNDEFINED,
        cv.UNDEFINED,
    ),
    # states C2, D2
    "charging": (
        EvseBinarySensorType.BINARY_SENSOR_CHARGING,
        DEVICE_CLASS_BATTERY_CHARGING,
        cv.UNDEFINED,
        cv.UNDEFINED,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        **{
            cv.Optional(key): binary_sensor.binary_sensor_schema(
                device_class=device_class,
                entity_category=entity_category,
                icon=icon,
            )
            for key, (_, device_class, entity_category, icon) in BINARY_SENSORS.items()
        },
    }
)


# fault sensors of optional features: the feature (evse key) they need
FEATURE_SENSORS = {
    "rcm_triggered": CONF_RCM,
    "rcm_selftest_fault": CONF_RCM,
    "temperature_high": CONF_TEMPERATURE_SENSORS,
    "temperature_fault": CONF_TEMPERATURE_SENSORS,
}


def _warn_unused_fault_sensors(config):
    full_config = fv.full_config.get()
    evse_path = full_config.get_path_for_id(config[CONF_EVSE_ID])[:-1]
    evse_config = full_config.get_config_for_path(evse_path)
    for key, feature in FEATURE_SENSORS.items():
        if key in config and feature not in evse_config:
            _LOGGER.warning(
                "evse binary sensor %s stays off: '%s' is not configured in evse",
                key,
                feature,
            )
    return config


FINAL_VALIDATE_SCHEMA = _warn_unused_fault_sensors


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    for key, (sensor_type, *_) in BINARY_SENSORS.items():
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            cg.add(hub.set_binary_sensor(sensor_type, var))
