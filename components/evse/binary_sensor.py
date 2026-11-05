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

from . import CONF_EVSE_ID, Evse, evse_ns

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


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    for key, (sensor_type, *_) in BINARY_SENSORS.items():
        if conf := config.get(key):
            var = await binary_sensor.new_binary_sensor(conf)
            cg.add(hub.set_binary_sensor(sensor_type, var))
