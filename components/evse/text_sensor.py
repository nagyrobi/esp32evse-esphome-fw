import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_EVSE_ID, Evse, evse_ns

DEPENDENCIES = ["evse"]

EvseTextSensorType = evse_ns.enum("EvseTextSensorType")

TEXT_SENSORS = {
    # J1772 state: A, B1, B2, C1, C2, D1, D2, E, F
    "state": (
        EvseTextSensorType.TEXT_SENSOR_STATE,
        text_sensor.text_sensor_schema(icon="mdi:ev-station"),
    ),
    # active errors, comma separated, "none" without errors
    "errors": (
        EvseTextSensorType.TEXT_SENSOR_ERRORS,
        text_sensor.text_sensor_schema(
            icon="mdi:alert-circle-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        **{cv.Optional(key): schema for key, (_, schema) in TEXT_SENSORS.items()},
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    for key, (sensor_type, _) in TEXT_SENSORS.items():
        if conf := config.get(key):
            var = await text_sensor.new_text_sensor(conf)
            cg.add(hub.set_text_sensor(sensor_type, var))
