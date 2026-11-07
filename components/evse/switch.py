import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_EVSE_ID, Evse, evse_ns

DEPENDENCIES = ["evse"]

EvseSwitch = evse_ns.class_("EvseSwitch", switch.Switch, cg.Parented.template(Evse))
EvseSwitchType = evse_ns.enum("EvseSwitchType")

# key: (type, icon, entity_category)
SWITCHES = {
    # allow charging (esp32-evse: enabled)
    "enable": (EvseSwitchType.SWITCH_ENABLE, "mdi:ev-plug-type2", cv.UNDEFINED),
    # when off, the EVSE goes to state F (esp32-evse: available)
    "available": (
        EvseSwitchType.SWITCH_AVAILABLE,
        "mdi:ev-station",
        ENTITY_CATEGORY_CONFIG,
    ),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        **{
            cv.Optional(key): switch.switch_schema(
                EvseSwitch,
                block_inverted=True,
                default_restore_mode="RESTORE_DEFAULT_ON",
                icon=icon,
                entity_category=entity_category,
            )
            for key, (_, icon, entity_category) in SWITCHES.items()
        },
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    for key, (switch_type, *_) in SWITCHES.items():
        if conf := config.get(key):
            var = await switch.new_switch(conf)
            await cg.register_parented(var, hub)
            cg.add(var.set_type(switch_type))
            cg.add(hub.set_switch(switch_type, var))
