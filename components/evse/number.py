from typing import NamedTuple

import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_MODE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_CONFIG,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_SECOND,
    UNIT_WATT,
    UNIT_WATT_HOURS,
)

from . import CONF_EVSE_ID, Evse, evse_ns

DEPENDENCIES = ["evse"]

EvseNumber = evse_ns.class_("EvseNumber", number.Number, cg.Parented.template(Evse))
EvseNumberType = evse_ns.enum("EvseNumberType")


class NumberSpec(NamedTuple):
    type: object
    min_value: float
    max_value: float
    step: float
    unit: str
    device_class: str
    icon: str
    entity_category: str = cv.UNDEFINED
    mode: str = cv.UNDEFINED  # default of the number mode (ESPHome default: AUTO)


_CURRENT = (UNIT_AMPERE, DEVICE_CLASS_CURRENT, "mdi:current-ac")
_ENERGY = (UNIT_WATT_HOURS, DEVICE_CLASS_ENERGY, "mdi:lightning-bolt")
_TIME = (UNIT_SECOND, DEVICE_CLASS_DURATION, "mdi:timer-outline")
_POWER = (UNIT_WATT, DEVICE_CLASS_POWER, "mdi:flash-triangle-outline")

# Runtime values (charging_current and the limits) are reset to their defaults after boot,
# the default_* values, the maximum charging current and the temperature threshold are
# persistent (like the NVS settings of esp32-evse).
NUMBERS = {
    "charging_current": NumberSpec(
        EvseNumberType.NUMBER_CHARGING_CURRENT, 6, 63, 0.1, *_CURRENT
    ),
    "default_charging_current": NumberSpec(
        EvseNumberType.NUMBER_DEFAULT_CHARGING_CURRENT,
        6,
        63,
        0.1,
        *_CURRENT,
        ENTITY_CATEGORY_CONFIG,
    ),
    "maximum_charging_current": NumberSpec(
        EvseNumberType.NUMBER_MAXIMUM_CHARGING_CURRENT,
        6,
        63,
        1,
        *_CURRENT,
        ENTITY_CATEGORY_CONFIG,
        mode="BOX",
    ),
    "consumption_limit": NumberSpec(
        EvseNumberType.NUMBER_CONSUMPTION_LIMIT, 0, 1000000, 1, *_ENERGY
    ),
    "default_consumption_limit": NumberSpec(
        EvseNumberType.NUMBER_DEFAULT_CONSUMPTION_LIMIT,
        0,
        1000000,
        1,
        *_ENERGY,
        ENTITY_CATEGORY_CONFIG,
    ),
    "charging_time_limit": NumberSpec(
        EvseNumberType.NUMBER_CHARGING_TIME_LIMIT, 0, 2592000, 1, *_TIME
    ),
    "default_charging_time_limit": NumberSpec(
        EvseNumberType.NUMBER_DEFAULT_CHARGING_TIME_LIMIT,
        0,
        2592000,
        1,
        *_TIME,
        ENTITY_CATEGORY_CONFIG,
    ),
    "under_power_limit": NumberSpec(
        EvseNumberType.NUMBER_UNDER_POWER_LIMIT, 0, 65535, 1, *_POWER
    ),
    "default_under_power_limit": NumberSpec(
        EvseNumberType.NUMBER_DEFAULT_UNDER_POWER_LIMIT,
        0,
        65535,
        1,
        *_POWER,
        ENTITY_CATEGORY_CONFIG,
    ),
    "temperature_threshold": NumberSpec(
        EvseNumberType.NUMBER_TEMPERATURE_THRESHOLD,
        40,
        80,
        1,
        UNIT_CELSIUS,
        DEVICE_CLASS_TEMPERATURE,
        "mdi:thermometer-alert",
        ENTITY_CATEGORY_CONFIG,
        mode="BOX",
    ),
}


def _number_schema(spec):
    schema = number.number_schema(
        EvseNumber,
        unit_of_measurement=spec.unit,
        device_class=spec.device_class,
        icon=spec.icon,
        entity_category=spec.entity_category,
    )
    if spec.mode is not cv.UNDEFINED:
        schema = schema.extend(
            {
                cv.Optional(CONF_MODE, default=spec.mode): cv.enum(
                    number.NUMBER_MODES, upper=True
                )
            }
        )
    return schema


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        **{cv.Optional(key): _number_schema(spec) for key, spec in NUMBERS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    for key, spec in NUMBERS.items():
        if conf := config.get(key):
            var = await number.new_number(
                conf,
                min_value=spec.min_value,
                max_value=spec.max_value,
                step=spec.step,
            )
            await cg.register_parented(var, hub)
            cg.add(var.set_type(spec.type))
            cg.add(hub.set_number(spec.type, var))
