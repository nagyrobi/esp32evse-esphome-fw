"""EVSE controller, re-implementation of esp32-evse (https://github.com/dzurikmiroslav/esp32-evse)."""

import logging

from esphome import pins
import esphome.codegen as cg

# Import the classes, not the modules: importing the platforms of this package
# (evse.binary_sensor, evse.sensor, evse.switch) rebinds those module names here.
from esphome.components.binary_sensor import BinarySensor
from esphome.components.light import LightState
from esphome.components.output import FloatOutput
from esphome.components.sensor import Sensor
from esphome.components.switch import Switch
from esphome.components.voltage_sampler import VoltageSampler
import esphome.config_validation as cv
from esphome.const import (
    CONF_FREQUENCY,
    CONF_ID,
    CONF_INPUT,
    CONF_MAX_POWER,
    CONF_MIN_POWER,
    CONF_OUTPUT,
    CONF_PIN,
    CONF_PULLDOWN,
    CONF_RESTORE_MODE,
    CONF_SENSOR,
)
import esphome.final_validate as fv

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@nagyrobi"]
AUTO_LOAD = ["voltage_sampler"]

CONF_EVSE_ID = "evse_id"
CONF_PILOT = "pilot"
CONF_LEVELS = "levels"
CONF_RELAY = "relay"
CONF_RCM = "rcm"
CONF_TEST_PIN = "test_pin"
CONF_CHARGING_LED = "charging_led"
CONF_ERROR_LED = "error_led"
CONF_WIFI_LED = "wifi_led"
CONF_AUTHORIZATION = "authorization"
CONF_AUTHORIZATION_TIMEOUT = "authorization_timeout"
CONF_ENERGY_METER = "energy_meter"
CONF_TEMPERATURE_SENSORS = "temperature_sensors"
CONF_TEMPERATURE_THRESHOLD = "temperature_threshold"
CONF_TEMPERATURE_TIMEOUT = "temperature_timeout"
CONF_MAXIMUM_CHARGING_CURRENT = "maximum_charging_current"
CONF_DEFAULT_CHARGING_CURRENT = "default_charging_current"
CONF_DEFAULT_CONSUMPTION_LIMIT = "default_consumption_limit"
CONF_DEFAULT_CHARGING_TIME_LIMIT = "default_charging_time_limit"
CONF_DEFAULT_UNDER_POWER_LIMIT = "default_under_power_limit"

evse_ns = cg.esphome_ns.namespace("evse")
Evse = evse_ns.class_("Evse", cg.PollingComponent)

PHASES = ("l1", "l2", "l3")
METER_CURRENTS = tuple(f"current_{phase}" for phase in PHASES)
METER_VOLTAGES = tuple(f"voltage_{phase}" for phase in PHASES)

LEVEL_NAMES = ("12V", "9V", "6V", "3V", "-12V")


def validate_levels(value):
    """Pilot thresholds in mV at the ADC input, same as `levels` of esp32-evse board.yaml."""
    value = cv.ensure_list(cv.int_range(min=0, max=3300))(value)
    if len(value) != len(LEVEL_NAMES):
        raise cv.Invalid(
            f"Exactly {len(LEVEL_NAMES)} levels in mV are required: "
            f"[{', '.join(LEVEL_NAMES)}]"
        )
    if any(a <= b for a, b in zip(value, value[1:], strict=False)):
        raise cv.Invalid("The levels must be in descending order")
    return value


def validate_max_current(value):
    value = cv.All(cv.current, cv.Range(min=6.0, max=63.0))(value)
    if value != int(value):
        raise cv.Invalid(
            "The maximum charging current must be a whole number of amperes"
        )
    return int(value)


energy_wh = cv.float_with_unit("energy", "(Wh)", optional_unit=True)
power_w = cv.float_with_unit("power", "(W)", optional_unit=True)


def validate_phases(config):
    """Either current_l1 and voltage_l1 (single phase) or all six sensors (three phase)."""
    l2_l3 = [key for key in METER_CURRENTS[1:] + METER_VOLTAGES[1:] if key in config]
    if l2_l3 and len(l2_l3) != 4:
        raise cv.Invalid(
            "Configure either current_l1 and voltage_l1 (single phase), or current_l1, "
            "current_l2, current_l3, voltage_l1, voltage_l2 and voltage_l3 (three phase)"
        )
    return config


# Any sensors in A and V, e.g. of evse_energy_meter or a third party meter (sdm_meter...).
ENERGY_METER_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(METER_CURRENTS[0]): cv.use_id(Sensor),
            cv.Optional(METER_CURRENTS[1]): cv.use_id(Sensor),
            cv.Optional(METER_CURRENTS[2]): cv.use_id(Sensor),
            cv.Required(METER_VOLTAGES[0]): cv.use_id(Sensor),
            cv.Optional(METER_VOLTAGES[1]): cv.use_id(Sensor),
            cv.Optional(METER_VOLTAGES[2]): cv.use_id(Sensor),
        }
    ),
    validate_phases,
)

PILOT_SCHEMA = cv.Schema(
    {
        # ledc output at 1kHz, 100% = +12V, 0% = -12V
        cv.Required(CONF_OUTPUT): cv.use_id(FloatOutput),
        # adc sensor of the pilot feedback (attenuation 12db)
        cv.Required(CONF_SENSOR): cv.use_id(VoltageSampler),
        cv.Required(CONF_LEVELS): validate_levels,
    }
)

RCM_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_PIN): pins.gpio_pin_schema(
            {CONF_INPUT: True, CONF_PULLDOWN: True}, internal=True
        ),
        cv.Required(CONF_TEST_PIN): pins.gpio_output_pin_schema,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Evse),
        cv.Required(CONF_PILOT): PILOT_SCHEMA,
        cv.Required(CONF_RELAY): cv.use_id(Switch),
        cv.Optional(CONF_RCM): RCM_SCHEMA,
        cv.Optional(CONF_CHARGING_LED): cv.use_id(LightState),
        cv.Optional(CONF_ERROR_LED): cv.use_id(LightState),
        cv.Optional(CONF_WIFI_LED): cv.use_id(LightState),
        # binary_sensor or switch, authorization is required only when configured
        cv.Optional(CONF_AUTHORIZATION): cv.use_id(cg.EntityBase),
        cv.Optional(
            CONF_AUTHORIZATION_TIMEOUT, default="60s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ENERGY_METER): ENERGY_METER_SCHEMA,
        cv.Optional(CONF_TEMPERATURE_SENSORS): cv.ensure_list(cv.use_id(Sensor)),
        cv.Optional(CONF_TEMPERATURE_THRESHOLD, default=60): cv.All(
            cv.temperature, cv.Range(min=40, max=80), cv.int_
        ),
        cv.Optional(
            CONF_TEMPERATURE_TIMEOUT, default="2min"
        ): cv.positive_time_period_milliseconds,
        # factory defaults of the settings, the number entities can change them at run time
        cv.Optional(CONF_MAXIMUM_CHARGING_CURRENT, default=32): validate_max_current,
        cv.Optional(CONF_DEFAULT_CHARGING_CURRENT): cv.All(
            cv.current, cv.Range(min=6.0, max=63.0)
        ),
        cv.Optional(CONF_DEFAULT_CONSUMPTION_LIMIT, default=0): cv.All(
            energy_wh, cv.Range(min=0, max=1000000)
        ),
        cv.Optional(
            CONF_DEFAULT_CHARGING_TIME_LIMIT, default="0s"
        ): cv.positive_time_period_seconds,
        cv.Optional(CONF_DEFAULT_UNDER_POWER_LIMIT, default=0): cv.All(
            power_w, cv.Range(min=0, max=65535)
        ),
    }
).extend(cv.polling_component_schema("5s"))


def _get_declaring_config(full_config, id_):
    """Path and config of the component declaring id_ (empty dict if it is not a mapping)."""
    path = full_config.get_path_for_id(id_)[:-1]
    declaring_config = full_config.get_config_for_path(path)
    return path, declaring_config if isinstance(declaring_config, dict) else {}


def _final_validate(config):
    full_config = fv.full_config.get()

    # The relay must start open after (re)boot, independent of the EVSE controller.
    _, relay_config = _get_declaring_config(full_config, config[CONF_RELAY])
    restore_mode = relay_config.get(CONF_RESTORE_MODE)
    if restore_mode is not None and str(restore_mode).upper() != "ALWAYS_OFF":
        raise cv.Invalid(
            f"The AC relay switch '{config[CONF_RELAY]}' must use "
            f"'restore_mode: ALWAYS_OFF' (now {restore_mode})",
            path=[CONF_RELAY],
        )

    # Pilot PWM is 1kHz, 100% must be a constant +12V.
    pilot = config[CONF_PILOT]
    _, output_config = _get_declaring_config(full_config, pilot[CONF_OUTPUT])
    if output_config.get("platform") == "ledc":
        frequency = output_config.get(CONF_FREQUENCY)
        if frequency is not None and abs(float(frequency) - 1000.0) > 0.5:
            raise cv.Invalid(
                f"The pilot ledc output '{pilot[CONF_OUTPUT]}' must use 'frequency: 1000Hz'",
                path=[CONF_PILOT, CONF_OUTPUT],
            )
    for key, default in ((CONF_MIN_POWER, 0.0), (CONF_MAX_POWER, 1.0)):
        if key in output_config and float(output_config[key]) != default:
            raise cv.Invalid(
                f"The pilot output '{pilot[CONF_OUTPUT]}' must not use '{key}'",
                path=[CONF_PILOT, CONF_OUTPUT],
            )

    # The levels are calibrated voltages in mV.
    _, adc_config = _get_declaring_config(full_config, pilot[CONF_SENSOR])
    if adc_config.get("platform") == "adc":
        if adc_config.get("raw"):
            raise cv.Invalid(
                f"The pilot adc sensor '{pilot[CONF_SENSOR]}' must not use 'raw: true'",
                path=[CONF_PILOT, CONF_SENSOR],
            )
        attenuation = adc_config.get("attenuation")
        if attenuation is not None and str(attenuation) != "12db":
            _LOGGER.warning(
                "The pilot adc sensor '%s' uses attenuation %s, the esp32-evse levels "
                "are measured with 12db",
                pilot[CONF_SENSOR],
                attenuation,
            )

    # The authorization source is a binary sensor or a switch.
    if (auth := config.get(CONF_AUTHORIZATION)) is not None:
        path, _ = _get_declaring_config(full_config, auth)
        if not path or path[0] not in ("binary_sensor", "switch"):
            raise cv.Invalid(
                f"'{auth}' must be the id of a binary_sensor or a switch",
                path=[CONF_AUTHORIZATION],
            )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pilot = config[CONF_PILOT]
    cg.add(var.set_pilot_output(await cg.get_variable(pilot[CONF_OUTPUT])))
    cg.add(var.set_pilot_sensor(await cg.get_variable(pilot[CONF_SENSOR])))
    cg.add(var.set_pilot_levels(*pilot[CONF_LEVELS]))

    cg.add(var.set_relay(await cg.get_variable(config[CONF_RELAY])))

    if rcm := config.get(CONF_RCM):
        pin = await cg.gpio_pin_expression(rcm[CONF_PIN])
        test_pin = await cg.gpio_pin_expression(rcm[CONF_TEST_PIN])
        cg.add(var.set_rcm_pins(pin, test_pin))

    for key, setter in (
        (CONF_CHARGING_LED, var.set_charging_led),
        (CONF_ERROR_LED, var.set_error_led),
        (CONF_WIFI_LED, var.set_wifi_led),
    ):
        if key in config:
            cg.add(setter(await cg.get_variable(config[key])))

    if (auth := config.get(CONF_AUTHORIZATION)) is not None:
        full_id, auth_var = await cg.get_variable_with_full_id(auth)
        if full_id.type.inherits_from(BinarySensor):
            cg.add(var.set_authorization_binary_sensor(auth_var))
        else:
            cg.add(var.set_authorization_switch(auth_var))
    cg.add(var.set_authorization_timeout(config[CONF_AUTHORIZATION_TIMEOUT]))

    if (meter := config.get(CONF_ENERGY_METER)) is not None:
        for phase, (current, voltage) in enumerate(
            zip(METER_CURRENTS, METER_VOLTAGES, strict=True)
        ):
            if current in meter:
                cg.add(
                    var.set_meter_current(phase, await cg.get_variable(meter[current]))
                )
            if voltage in meter:
                cg.add(
                    var.set_meter_voltage(phase, await cg.get_variable(meter[voltage]))
                )

    for temperature_sensor in config.get(CONF_TEMPERATURE_SENSORS, []):
        cg.add(var.add_temperature_sensor(await cg.get_variable(temperature_sensor)))
    cg.add(var.set_temperature_timeout(config[CONF_TEMPERATURE_TIMEOUT]))
    cg.add(var.set_temperature_threshold(config[CONF_TEMPERATURE_THRESHOLD]))

    cg.add(var.set_max_charging_current(config[CONF_MAXIMUM_CHARGING_CURRENT]))
    if CONF_DEFAULT_CHARGING_CURRENT in config:
        cg.add(var.set_default_charging_current(config[CONF_DEFAULT_CHARGING_CURRENT]))
    cg.add(
        var.set_default_consumption_limit(int(config[CONF_DEFAULT_CONSUMPTION_LIMIT]))
    )
    cg.add(
        var.set_default_charging_time_limit(
            int(config[CONF_DEFAULT_CHARGING_TIME_LIMIT].total_seconds)
        )
    )
    cg.add(
        var.set_default_under_power_limit(int(config[CONF_DEFAULT_UNDER_POWER_LIMIT]))
    )
