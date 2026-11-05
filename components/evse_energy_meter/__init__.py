"""Energy meter of the EVSE (port of the esp32-evse energy meter)."""

import logging

import esphome.codegen as cg
from esphome.components.sensor import new_sensor, sensor_schema
from esphome.components.switch import Switch
from esphome.components.voltage_sampler import VoltageSampler
import esphome.config_validation as cv
from esphome.const import (
    CONF_FILTERS,
    CONF_ID,
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
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@nagyrobi"]
AUTO_LOAD = ["sensor", "voltage_sampler"]

CONF_EVSE_ENERGY_METER_ID = "evse_energy_meter_id"
CONF_ADC_SENSOR = "adc_sensor"
CONF_MONITORED_RELAY = "monitored_relay"
CONF_SAMPLE_DURATION = "sample_duration"
CONF_TOTAL_POWER = "total_power"
CONF_TOTAL_ENERGY = "total_energy"

PHASES = ("l1", "l2", "l3")
CURRENTS = tuple(f"current_{phase}" for phase in PHASES)
VOLTAGES = tuple(f"voltage_{phase}" for phase in PHASES)

evse_energy_meter_ns = cg.esphome_ns.namespace("evse_energy_meter")
EvseEnergyMeter = evse_energy_meter_ns.class_("EvseEnergyMeter", cg.PollingComponent)

# The raw value of the current and voltage sensors is the RMS voltage at the ADC input in
# mV, their filters convert it to A / V (the esp32-evse `scale` is a `multiply` filter).
_ADC_INPUT = {cv.Required(CONF_ADC_SENSOR): cv.use_id(VoltageSampler)}

CURRENT_SCHEMA = sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(_ADC_INPUT)

VOLTAGE_SCHEMA = sensor_schema(
    unit_of_measurement=UNIT_VOLT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_VOLTAGE,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(_ADC_INPUT)


def validate_phases(config):
    """Either current_l1 and voltage_l1 (single phase) or all six (three phase)."""
    l2_l3 = [key for key in CURRENTS[1:] + VOLTAGES[1:] if key in config]
    if l2_l3 and len(l2_l3) != 4:
        raise cv.Invalid(
            "Configure either current_l1 and voltage_l1 (single phase), or current_l1, "
            "current_l2, current_l3, voltage_l1, voltage_l2 and voltage_l3 (three phase)"
        )
    return config


def warn_without_filters(config):
    for key in CURRENTS + VOLTAGES:
        if key in config and not config[key].get(CONF_FILTERS):
            _LOGGER.warning(
                "evse_energy_meter %s has no filters: its raw value is the RMS voltage at "
                "the ADC input in mV, convert it with e.g. 'filters: [multiply: <scale>]' "
                "using the scale of the esp32-evse board.yaml",
                key,
            )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EvseEnergyMeter),
            cv.Required(CURRENTS[0]): CURRENT_SCHEMA,
            cv.Optional(CURRENTS[1]): CURRENT_SCHEMA,
            cv.Optional(CURRENTS[2]): CURRENT_SCHEMA,
            cv.Required(VOLTAGES[0]): VOLTAGE_SCHEMA,
            cv.Optional(VOLTAGES[1]): VOLTAGE_SCHEMA,
            cv.Optional(VOLTAGES[2]): VOLTAGE_SCHEMA,
            cv.Optional(CONF_TOTAL_POWER): sensor_schema(
                unit_of_measurement=UNIT_WATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_POWER,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TOTAL_ENERGY): sensor_schema(
                unit_of_measurement=UNIT_WATT_HOURS,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_ENERGY,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            # optional: the currents are measured only while this switch (the AC relay) is
            # on, otherwise they are 0, so the noise of the current inputs is not counted
            # (the noise can be removed with sensor filters too)
            cv.Optional(CONF_MONITORED_RELAY): cv.use_id(Switch),
            cv.Optional("relay"): cv.invalid(
                "'relay' was renamed to 'monitored_relay'"
            ),
            cv.Optional(CONF_SAMPLE_DURATION, default="40ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=10),
                    max=cv.TimePeriod(milliseconds=100),
                ),
            ),
        }
    ).extend(cv.polling_component_schema("1s")),
    validate_phases,
    warn_without_filters,
)


def _validate_adc_inputs(config):
    """The inputs must provide calibrated voltages."""
    full_config = fv.full_config.get()
    for key in CURRENTS + VOLTAGES:
        if key not in config:
            continue
        input_id = config[key][CONF_ADC_SENSOR]
        path = full_config.get_path_for_id(input_id)[:-1]
        input_config = full_config.get_config_for_path(path)
        if (
            isinstance(input_config, dict)
            and input_config.get("platform") == "adc"
            and input_config.get("raw")
        ):
            raise cv.Invalid(
                f"The adc sensor '{input_id}' must not use 'raw: true', the energy "
                "meter needs calibrated voltages",
                path=[key, CONF_ADC_SENSOR],
            )
    return config


FINAL_VALIDATE_SCHEMA = _validate_adc_inputs


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_sample_duration(config[CONF_SAMPLE_DURATION]))

    for phase, (current, voltage) in enumerate(zip(CURRENTS, VOLTAGES, strict=True)):
        if conf := config.get(current):
            sens = await new_sensor(conf)
            adc_sensor = await cg.get_variable(conf[CONF_ADC_SENSOR])
            cg.add(var.set_current(phase, sens, adc_sensor))
        if conf := config.get(voltage):
            sens = await new_sensor(conf)
            adc_sensor = await cg.get_variable(conf[CONF_ADC_SENSOR])
            cg.add(var.set_voltage(phase, sens, adc_sensor))

    if conf := config.get(CONF_TOTAL_POWER):
        cg.add(var.set_total_power_sensor(await new_sensor(conf)))
    if conf := config.get(CONF_TOTAL_ENERGY):
        cg.add(var.set_total_energy_sensor(await new_sensor(conf)))

    if (relay := config.get(CONF_MONITORED_RELAY)) is not None:
        cg.add(var.set_monitored_relay(await cg.get_variable(relay)))
