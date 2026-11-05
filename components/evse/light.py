from esphome import pins
import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_OUTPUT_ID, CONF_PIN, CONF_TYPE
import esphome.final_validate as fv

from . import CONF_EVSE_ID, Evse, evse_ns

DEPENDENCIES = ["evse"]

EvseLightOutput = evse_ns.class_("EvseLightOutput", light.LightOutput, cg.Component)

# light types and the setters of the Evse hub
LIGHT_TYPES = {
    # off: A, E, F; blink 500/500ms: B; blink 1900/100ms: C1, D1; on: C2, D2
    "charging": "set_charging_led",
    # on: E, blink 500/500ms: F, off: otherwise
    "error": "set_error_led",
}

CONFIG_SCHEMA = light.BINARY_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(EvseLightOutput),
        cv.GenerateID(CONF_EVSE_ID): cv.use_id(Evse),
        cv.Required(CONF_TYPE): cv.one_of(*LIGHT_TYPES, lower=True),
        cv.Required(CONF_PIN): pins.gpio_output_pin_schema,
    }
)


def _validate_unique_type(config):
    lights = fv.full_config.get().get(light.DOMAIN, [])
    same = [
        conf
        for conf in lights
        if conf.get("platform") == "evse"
        and conf.get(CONF_TYPE) == config[CONF_TYPE]
        and conf.get(CONF_EVSE_ID) == config[CONF_EVSE_ID]
    ]
    if len(same) > 1:
        raise cv.Invalid(
            f"Only one evse light of type '{config[CONF_TYPE]}' is allowed"
        )
    return config


FINAL_VALIDATE_SCHEMA = _validate_unique_type


async def to_code(config):
    hub = await cg.get_variable(config[CONF_EVSE_ID])
    output = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    cg.add(output.set_pin(await cg.gpio_pin_expression(config[CONF_PIN])))
    await cg.register_component(output, config)
    await light.register_light(output, config)
    light_state = await cg.get_variable(config[CONF_ID])
    cg.add(getattr(hub, LIGHT_TYPES[config[CONF_TYPE]])(light_state))
