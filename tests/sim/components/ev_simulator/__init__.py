"""Test only: simulated vehicle and ADC inputs for running the EVSE on the ESPHome host platform."""

import esphome.codegen as cg
from esphome.components.output import FloatOutput
from esphome.components.switch import Switch
from esphome.components.voltage_sampler import VoltageSampler
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["output", "voltage_sampler"]

CONF_RELAY = "relay"
CONF_PILOT_OUTPUT_ID = "pilot_output_id"
CONF_PILOT_SENSOR_ID = "pilot_sensor_id"
CONF_CURRENT_IDS = "current_ids"
CONF_VOLTAGE_IDS = "voltage_ids"

ev_simulator_ns = cg.esphome_ns.namespace("ev_simulator")
EvSimulator = ev_simulator_ns.class_("EvSimulator", cg.Component)
PilotOutput = ev_simulator_ns.class_("PilotOutput", FloatOutput)
PilotSampler = ev_simulator_ns.class_("PilotSampler", VoltageSampler)
WaveSampler = ev_simulator_ns.class_("WaveSampler", VoltageSampler)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EvSimulator),
        cv.Required(CONF_RELAY): cv.use_id(Switch),
        cv.Required(CONF_PILOT_OUTPUT_ID): cv.declare_id(PilotOutput),
        cv.Required(CONF_PILOT_SENSOR_ID): cv.declare_id(PilotSampler),
        cv.Required(CONF_CURRENT_IDS): cv.All(
            [cv.declare_id(WaveSampler)], cv.Length(min=3, max=3)
        ),
        cv.Required(CONF_VOLTAGE_IDS): cv.All(
            [cv.declare_id(WaveSampler)], cv.Length(min=3, max=3)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_relay(await cg.get_variable(config[CONF_RELAY])))

    pilot_output = cg.new_Pvariable(config[CONF_PILOT_OUTPUT_ID])
    cg.add(var.set_pilot_output(pilot_output))

    pilot_sensor = cg.new_Pvariable(config[CONF_PILOT_SENSOR_ID])
    cg.add(pilot_sensor.set_parent(var))

    for is_current, key in ((True, CONF_CURRENT_IDS), (False, CONF_VOLTAGE_IDS)):
        for phase, sampler_id in enumerate(config[key]):
            sampler = cg.new_Pvariable(sampler_id)
            cg.add(sampler.set_parent(var))
            cg.add(sampler.set_input(is_current, phase))
