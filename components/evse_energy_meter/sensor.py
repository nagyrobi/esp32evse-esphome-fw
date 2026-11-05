import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_POWER,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_VOLT,
    UNIT_WATT,
    UNIT_WATT_HOURS,
)

from . import CONF_EVSE_ENERGY_METER_ID, EvseEnergyMeter

DEPENDENCIES = ["evse_energy_meter"]

CONF_TOTAL_ENERGY = "total_energy"
PHASES = ("l1", "l2", "l3")

_VOLTAGE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_VOLTAGE,
    state_class=STATE_CLASS_MEASUREMENT,
)
_CURRENT_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ENERGY_METER_ID): cv.use_id(EvseEnergyMeter),
        **{cv.Optional(f"voltage_{phase}"): _VOLTAGE_SCHEMA for phase in PHASES},
        **{cv.Optional(f"current_{phase}"): _CURRENT_SCHEMA for phase in PHASES},
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_TOTAL_ENERGY): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT_HOURS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ENERGY_METER_ID])

    for index, phase in enumerate(PHASES):
        if conf := config.get(f"voltage_{phase}"):
            sens = await sensor.new_sensor(conf)
            cg.add(hub.set_voltage_sensor(index, sens))
        if conf := config.get(f"current_{phase}"):
            sens = await sensor.new_sensor(conf)
            cg.add(hub.set_current_sensor(index, sens))

    if conf := config.get(CONF_POWER):
        sens = await sensor.new_sensor(conf)
        cg.add(hub.set_power_sensor(sens))

    if conf := config.get(CONF_TOTAL_ENERGY):
        sens = await sensor.new_sensor(conf)
        cg.add(hub.set_total_energy_sensor(sens))
