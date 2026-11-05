import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_EVSE_ENERGY_METER_ID, EvseEnergyMeter, evse_energy_meter_ns

DEPENDENCIES = ["evse_energy_meter"]

CONF_RESET_TOTAL_ENERGY = "reset_total_energy"

ResetTotalEnergyButton = evse_energy_meter_ns.class_(
    "ResetTotalEnergyButton", button.Button, cg.Parented.template(EvseEnergyMeter)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ENERGY_METER_ID): cv.use_id(EvseEnergyMeter),
        cv.Optional(CONF_RESET_TOTAL_ENERGY): button.button_schema(
            ResetTotalEnergyButton,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:counter",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ENERGY_METER_ID])
    if conf := config.get(CONF_RESET_TOTAL_ENERGY):
        btn = await button.new_button(conf)
        await cg.register_parented(btn, hub)
