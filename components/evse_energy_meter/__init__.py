"""Energy meter of the EVSE (port of the esp32-evse energy meter)."""

import logging

import esphome.codegen as cg
from esphome.components import voltage_sampler
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SENSORS
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@nagyrobi"]
AUTO_LOAD = ["voltage_sampler"]

CONF_EVSE_ENERGY_METER_ID = "evse_energy_meter_id"
CONF_THREE_PHASE = "three_phase"
CONF_AC_VOLTAGE = "ac_voltage"
CONF_CURRENT = "current"
CONF_VOLTAGE = "voltage"
CONF_SCALE = "scale"
CONF_SAMPLE_DURATION = "sample_duration"

evse_energy_meter_ns = cg.esphome_ns.namespace("evse_energy_meter")
EvseEnergyMeter = evse_energy_meter_ns.class_("EvseEnergyMeter", cg.PollingComponent)


def _one_or_three(value):
    if len(value) not in (1, 3):
        raise cv.Invalid(
            "Specify 1 input (L1, also used for L2/L3 on three phases) or 3 inputs (L1, L2, L3)"
        )
    return value


INPUTS_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SENSORS): cv.All(
            cv.ensure_list(cv.use_id(voltage_sampler.VoltageSampler)), _one_or_three
        ),
        # multiplier of the RMS value in mV at the ADC input, same as in esp32-evse board.yaml
        cv.Required(CONF_SCALE): cv.positive_float,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EvseEnergyMeter),
        cv.Optional(CONF_THREE_PHASE, default=False): cv.boolean,
        cv.Optional(CONF_AC_VOLTAGE, default="230V"): cv.All(
            cv.voltage, cv.Range(min=100.0, max=300.0)
        ),
        cv.Optional(CONF_CURRENT): INPUTS_SCHEMA,
        cv.Optional(CONF_VOLTAGE): INPUTS_SCHEMA,
        cv.Optional(CONF_SAMPLE_DURATION, default="40ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(
                min=cv.TimePeriod(milliseconds=10), max=cv.TimePeriod(milliseconds=100)
            ),
        ),
    }
).extend(cv.polling_component_schema("1s"))


def _validate_adc_inputs(config):
    """The scale factors expect calibrated voltages from `adc` sensors."""
    full_config = fv.full_config.get()
    for key in (CONF_CURRENT, CONF_VOLTAGE):
        if key not in config:
            continue
        for input_id in config[key][CONF_SENSORS]:
            path = full_config.get_path_for_id(input_id)[:-1]
            input_config = full_config.get_config_for_path(path)
            if (
                not isinstance(input_config, dict)
                or input_config.get("platform") != "adc"
            ):
                continue
            if input_config.get("raw"):
                raise cv.Invalid(
                    f"The adc sensor '{input_id}' must not use 'raw: true', the energy "
                    "meter needs calibrated voltages"
                )
    return config


FINAL_VALIDATE_SCHEMA = _validate_adc_inputs


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_EVSE_ENERGY_METER")

    cg.add(var.set_three_phase(config[CONF_THREE_PHASE]))
    cg.add(var.set_ac_voltage(config[CONF_AC_VOLTAGE]))
    cg.add(var.set_sample_duration(config[CONF_SAMPLE_DURATION]))

    if current := config.get(CONF_CURRENT):
        for input_id in current[CONF_SENSORS]:
            cg.add(var.add_current_input(await cg.get_variable(input_id)))
        cg.add(var.set_current_scale(current[CONF_SCALE]))

    if voltage := config.get(CONF_VOLTAGE):
        for input_id in voltage[CONF_SENSORS]:
            cg.add(var.add_voltage_input(await cg.get_variable(input_id)))
        cg.add(var.set_voltage_scale(voltage[CONF_SCALE]))
