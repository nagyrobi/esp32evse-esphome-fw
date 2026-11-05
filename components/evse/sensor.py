import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MILLIVOLT,
    UNIT_SECOND,
    UNIT_WATT_HOURS,
)

from . import CONF_EVSE_ID, Evse, evse_ns

DEPENDENCIES = ["evse"]

EvseSensorType = evse_ns.enum("EvseSensorType")

_DURATION = {
    "unit_of_measurement": UNIT_SECOND,
    "accuracy_decimals": 0,
    "device_class": DEVICE_CLASS_DURATION,
    "state_class": STATE_CLASS_MEASUREMENT,
    "icon": "mdi:timer-outline",
}
_PILOT = {
    "unit_of_measurement": UNIT_MILLIVOLT,
    "accuracy_decimals": 0,
    "device_class": DEVICE_CLASS_VOLTAGE,
    "state_class": STATE_CLASS_MEASUREMENT,
    "entity_category": ENTITY_CATEGORY_DIAGNOSTIC,
}

SENSORS = {
    # energy of the session, requires an energy meter
    "session_energy": (
        EvseSensorType.SENSOR_SESSION_ENERGY,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT_HOURS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
    ),
    # time since the vehicle connected
    "session_time": (
        EvseSensorType.SENSOR_SESSION_TIME,
        sensor.sensor_schema(**_DURATION),
    ),
    # time with closed relay in the session
    "charging_time": (
        EvseSensorType.SENSOR_CHARGING_TIME,
        sensor.sensor_schema(**_DURATION),
    ),
    # measured pilot voltages at the ADC input, useful to calibrate the levels
    "pilot_high_voltage": (
        EvseSensorType.SENSOR_PILOT_HIGH_VOLTAGE,
        sensor.sensor_schema(**_PILOT),
    ),
    "pilot_low_voltage": (
        EvseSensorType.SENSOR_PILOT_LOW_VOLTAGE,
        sensor.sensor_schema(**_PILOT),
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        **{cv.Optional(key): schema for key, (_, schema) in SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    for key, (sensor_type, _) in SENSORS.items():
        if conf := config.get(key):
            var = await sensor.new_sensor(conf)
            cg.add(hub.set_sensor(sensor_type, var))
