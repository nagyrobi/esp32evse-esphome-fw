import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_EVSE_ID, Evse, evse_ns

DEPENDENCIES = ["evse"]

CONF_CLEAR_ERROR = "clear_error"

ClearErrorButton = evse_ns.class_(
    "ClearErrorButton", button.Button, cg.Parented.template(Evse)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        # leave the error state now instead of after 60s (pilot fault, diode short, RCM)
        cv.Optional(CONF_CLEAR_ERROR): button.button_schema(
            ClearErrorButton,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:backup-restore",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    if conf := config.get(CONF_CLEAR_ERROR):
        btn = await button.new_button(conf)
        await cg.register_parented(btn, hub)
