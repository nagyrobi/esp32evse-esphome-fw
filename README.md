# ESPHome firmware for esp32-evse boards

ESPHome re-implementation of the EVSE (electric vehicle charger) controller of
[esp32-evse](https://github.com/dzurikmiroslav/esp32-evse), as two
[external components](https://esphome.io/components/external_components/):

| Component | Ported from esp32-evse | What it does |
|---|---|---|
| `evse` | `components/evse` (state machine), `pilot.c`, `rcm.c`, LED handling of `main.c` | J1772 / IEC 61851 state machine, control pilot, AC relay, residual current monitor, LEDs, authorization, limits, temperature protection |
| `evse_energy_meter` | `energy_meter.c` | Current and voltage sensors (true RMS of ADC inputs, single or three phase), total power, persistent total energy |

The `evse` takes the current and voltage of the phases from sensors, so it works with
`evse_energy_meter` or with any other ESPHome energy meter.

Everything else of the board (aux inputs / outputs, analog input, UARTs / RS-485,
1-Wire temperature sensors, button) uses standard ESPHome components. Socket lock,
proximity, scheduler, AT commands, Nextion, Lua scripts and the Modbus / REST
servers of esp32-evse are not ported.

The example configuration [`esp32s2da.yaml`](esp32s2da.yaml) is a complete
firmware for the **ESP32-S2-DA** board (`board-config/esp32s2da.yaml` of esp32-evse).
It is validated and compiled with ESPHome 2026.10 (ESP-IDF), and the firmware logic is
exercised in a simulation on the ESPHome host platform ([`tests/sim`](tests/sim)).
**It has not been tested on the real hardware yet**, see [Safety](#safety).

The ADC inputs (pilot feedback, current, voltage) are standard `adc` sensors that the
components sample through ESPHome's `VoltageSampler` interface. This way they share the
ADC unit with any other `adc` sensor (e.g. the IN3 analog input) and ESPHome handles
the pins, attenuation and calibration.

## Quick start

1. Copy [`esp32s2da.yaml`](esp32s2da.yaml) into your ESPHome configuration directory
   and create the referenced secrets (see [`secrets.example.yaml`](secrets.example.yaml)).
2. Review the board specific values (marked in the file), especially the phases of the
   energy meter (single or three), `maximum_charging_current`, the pilot `levels` and the
   `multiply` filters of the energy meter (the `scale` values of your esp32-evse
   `board.yaml`).
3. Flash the first time over USB / serial (the ESPHome partition table differs from
   esp32-evse), later updates go over the air. All esp32-evse settings are lost.

```yaml
external_components:
  - source: github://nagyrobi/esp32evse-esphome-fw@main
    components: [evse, evse_energy_meter]
```

## esp32-evse board.yaml to ESPHome

On the ESP32-S2 the ADC1 channel *N* is `GPIO(N+1)`.

| esp32-evse `board.yaml` | ESPHome (`esp32s2da.yaml`) |
|---|---|
| `pilot.gpio: 33` | `ledc` output `pilot_pwm` on GPIO33, 1000 Hz |
| `pilot.adcChannel: 3` | `adc` sensor `pilot_adc` on GPIO4, `attenuation: 12db` |
| `pilot.levels` | `evse.pilot.levels` (same values) |
| `acRelay.gpios: [21]` | `gpio` switch `ac_relay` on GPIO21, `restore_mode: ALWAYS_OFF` |
| `rcm.gpio: 41`, `rcm.testGpio: 26` | `evse.rcm.pin` / `evse.rcm.test_pin` |
| `leds.charging` / `leds.error` | `evse` lights (`type: charging` / `type: error`) on GPIO36 / GPIO37 |
| `leds.wifi` | `status_led` light on GPIO35, inverted (on while connected) |
| `energyMeter.current.adcChannels: [4, 5, 6]`, `scale` | `adc` sensors on GPIO5, 6, 7, `evse_energy_meter` `current_l1` … `current_l3` with a `multiply: <scale>` filter |
| `energyMeter.voltage.adcChannels: [7, 8, 9]`, `scale` | `adc` sensors on GPIO8, 9, 10, `evse_energy_meter` `voltage_l1` … `voltage_l3` with a `multiply: <scale>` filter |
| `button.gpio: 0` | `gpio` binary sensor, hold 10 s for factory reset |
| `aux.inputs` IN1 / IN2 | `gpio` binary sensors on GPIO11 / GPIO2 |
| `aux.outputs` OUT1..OUT4 | `gpio` switches on GPIO17 / 16 / 15 / 14 |
| `aux.analogInputs` IN3 | `adc` sensor on GPIO1 |
| `serials` UART / RS-485 | `uart` buses (RS-485 DE/RE is GPIO39, e.g. `modbus.flow_control_pin`) |
| `onewire` | `one_wire` bus on GPIO42 + `dallas_temp` sensor |

## `evse` component

```yaml
evse:
  pilot:
    output: pilot_pwm        # FloatOutput (ledc, frequency: 1000Hz)
    sensor: pilot_adc        # adc sensor (attenuation: 12db, update_interval: never)
    levels: [2405, 2099, 1792, 1484, 728]
  relay: ac_relay            # switch, restore_mode: ALWAYS_OFF
  rcm:                       # optional
    pin: GPIO41
    test_pin: GPIO26
  authorization: rfid_ok     # optional binary_sensor or switch
  energy_meter:              # optional, current (A) and voltage (V) sensors of any meter
    current_l1: current_l1
    voltage_l1: voltage_l1
  temperature_sensors: [evse_temperature]  # optional
```

| Option | Default | Description |
|---|---|---|
| `pilot.output` | required | Output driving the pilot: 100 % = +12 V, 0 % = −12 V, PWM for the charging current. Must be 1 kHz (checked for `ledc`), no `min_power`/`max_power`. |
| `pilot.sensor` | required | `adc` sensor of the pilot feedback. It is sampled 250 times per measurement, use `update_interval: never` and no name. |
| `pilot.levels` | required | Five thresholds in mV at the ADC input: `[12V, 9V, 6V, 3V, -12V]`, identical to esp32-evse. Use the `pilot_high_voltage` / `pilot_low_voltage` sensors to calibrate. |
| `relay` | required | Switch of the AC contactor. Only the EVSE switches it, external changes are reverted. Must use `restore_mode: ALWAYS_OFF`. |
| `rcm` | – | Optional residual current monitor. Without it the RCM self test and monitoring are skipped completely, like esp32-evse on a board without RCM; the `rcm_triggered` / `rcm_selftest_fault` binary sensors then stay off (with a configuration warning). |
| `rcm.pin` | required | Residual current monitor output, rising edge interrupt, input with pull-down by default. |
| `rcm.test_pin` | required | Residual current monitor test input. The self test runs once per session when the vehicle connects; charging waits until it passes (500 ms timeout). |
| `authorization` | – | Binary sensor or switch. Authorization is required only when this is set. A session is authorized when the vehicle connects while the source is on, or within `authorization_timeout` after it turned on (e.g. an RFID tag shown before plugging in). The authorization is valid until the vehicle disconnects. |
| `authorization_timeout` | `60s` | Validity of an authorization made before the vehicle connects. |
| `energy_meter` | – | Current (A) and voltage (V) sensors of the phases, from `evse_energy_meter` or any other meter: `current_l1` and `voltage_l1` (single phase), or `current_l1` … `current_l3` and `voltage_l1` … `voltage_l3` (three phase). The EVSE calculates the power (sum of V × I) and the session energy from their filtered values while the relay is closed. Needed for the session energy, the consumption limit and the under power limit. |
| `temperature_sensors` | – | Sensors (e.g. `dallas_temp`), enables the `temperature_high` / `temperature_fault` errors. |
| `temperature_threshold` | `60°C` | Factory default of the temperature limit (40–80 °C). |
| `temperature_timeout` | `2min` | `temperature_fault` when a sensor has no valid value for this time. |
| `maximum_charging_current` | `32A` | Factory default of the maximum charging current (6–63 A). |
| `default_charging_current` | maximum | Factory default of the charging current after boot. |
| `default_consumption_limit` | `0` | Factory default of the consumption limit in Wh (0 = no limit). |
| `default_charging_time_limit` | `0s` | Factory default of the charging time limit (0 = no limit). |
| `default_under_power_limit` | `0` | Factory default of the under power limit in W (0 = no limit). |
| `update_interval` | `5s` | Publish interval of the session sensors. The state machine runs every 50 ms. |

Each state machine iteration measures the pilot with 250 ADC samples, like esp32-evse.
The duration of one measurement is logged at boot (`Pilot measurement: 250 samples in
… us`); the samples should span a few 1 ms PWM periods.

### Entities

All entities are optional, add only what you need.

**Numbers** (`platform: evse`). Like in esp32-evse, the runtime values start from
their defaults after boot; the `default_*` values, the maximum charging current and the
temperature threshold are persistent (the YAML values above are their factory defaults).

| Key | Unit | Range | Persistent |
|---|---|---|---|
| `charging_current` | A | 6–63 (≤ maximum), step 0.1 | no |
| `default_charging_current` | A | 6–63 (≤ maximum), step 0.1 | yes |
| `maximum_charging_current` | A | 6–63 | yes |
| `consumption_limit` | Wh | 0–1000000, 0 = off | no |
| `default_consumption_limit` | Wh | 0–1000000 | yes |
| `charging_time_limit` | s | 0–2592000, 0 = off | no |
| `default_charging_time_limit` | s | 0–2592000 | yes |
| `under_power_limit` | W | 0–65535, 0 = off | no |
| `default_under_power_limit` | W | 0–65535 | yes |
| `temperature_threshold` | °C | 40–80 | yes |

`maximum_charging_current` and `temperature_threshold` are input boxes (`mode: box`) by
default, the others use the ESPHome default (`mode: auto`).

**Limits.** While a limit of the session is reached, the `limit_reached` binary sensor is
on and charging is stopped (C2 → C1). 0 turns a limit off, and a new session starts
without reached limits.

* `consumption_limit`: the session energy is above the limit.
* `charging_time_limit`: the charging time of the session is above the limit.
* `under_power_limit`: while charging, the power is below the limit for 60 s. It applies
  only after the power was above the limit in the session, so a vehicle that has not
  started to draw power yet (or never draws that much) is not stopped. The 60 s start
  again each time charging starts, so the vehicle can ramp up after a pause. Unknown
  power values are ignored.

Raising the consumption or the charging time limit (or setting it to 0) allows charging
again. A reached under power limit stays until the end of the session (vehicle unplugged).

**Switches** (`platform: evse`): `enable` (allow charging) and `available` (off = state F,
config entity category by default). Default `restore_mode: RESTORE_DEFAULT_ON`, use
`ALWAYS_ON` to get the esp32-evse behaviour (always enabled after boot).

**Text sensors** (`platform: evse`): `state` (`A`, `B1`, `B2`, `C1`, `C2`, `D1`, `D2`,
`E`, `F`) and `errors` (comma separated error codes or `none`).

**Binary sensors** (`platform: evse`): the error codes `pilot_fault`, `diode_short`,
`rcm_triggered`, `rcm_selftest_fault`, `temperature_high`, `temperature_fault` and
`error` (any error), in the diagnostic entity category by default; and `limit_reached`,
`pending_authorization`, `vehicle_connected` (states B1–D2), `charging` (C2, D2).

**Sensors** (`platform: evse`): `session_energy` (Wh), `session_time` (s),
`charging_time` (s), and for diagnostics / level calibration `pilot_high_voltage`,
`pilot_low_voltage` (mV at the ADC input).

**Lights** (`platform: evse`): the LEDs driven by the EVSE, directly on a pin (no `output`
needed), one light per LED with `type: charging` or `type: error` (patterns in
[States](#states)). They are standard binary lights (`name`, `internal`, triggers...).

```yaml
light:
  - platform: evse
    type: charging
    id: led_charging
    pin: GPIO36
  - platform: evse
    type: error
    id: led_error
    pin: GPIO37
  # the WiFi LED is a plain ESPHome status LED
  - platform: status_led
    id: led_wifi
    pin:
      number: GPIO35
      inverted: true
```

With the inverted pin, the WiFi LED is on while WiFi and Home Assistant are connected
(like esp32-evse), goes off for 250 ms every 1.5 s while they are not connected (ESPHome
warning), and blinks fast on errors.

**Button** (`platform: evse`, diagnostic entity category by default): `clear_error` leaves
the error state now instead of after 60 s (pilot fault, diode short, RCM triggered, RCM
self test fault). The state machine restarts from state A, so a fault that is still
present trips again, and after an RCM error the RCM self test runs again before charging.
Temperature errors are not cleared, they clear when the temperature is back to normal.

### Third party energy meter

Any sensors in A and V can feed the EVSE, e.g. an SDM630 over Modbus:

```yaml
sensor:
  - platform: sdm_meter
    phase_a:
      current: {id: sdm_current_l1, name: Current L1}
      voltage: {id: sdm_voltage_l1, name: Voltage L1}
    phase_b:
      current: {id: sdm_current_l2, name: Current L2}
      voltage: {id: sdm_voltage_l2, name: Voltage L2}
    phase_c:
      current: {id: sdm_current_l3, name: Current L3}
      voltage: {id: sdm_voltage_l3, name: Voltage L3}

evse:
  energy_meter:
    current_l1: sdm_current_l1
    current_l2: sdm_current_l2
    current_l3: sdm_current_l3
    voltage_l1: sdm_voltage_l1
    voltage_l2: sdm_voltage_l2
    voltage_l3: sdm_voltage_l3
```

The session energy is integrated from the latest values, so its accuracy depends on the
update interval of the meter; the energy counter of the meter itself is the reference
for billing. Without current measurement, template sensors can provide estimated values.

### Filters

The EVSE calculates with the **filtered** values of the sensors it references:

* `energy_meter`: the power and the session energy (and so the consumption and under
  power limits) use the filtered current and voltage values. Calibration filters
  (`multiply`, `calibrate_linear`, ...) belong to these sensors. A throttling filter also
  slows down the power updates of the EVSE.
* `temperature_sensors`: a smoothing filter delays the over temperature error, a filter
  that drops values may cause `temperature_fault` after `temperature_timeout`.
* `authorization`: the filtered state of the binary sensor (e.g. `delayed_on`).
* The `adc` sensors of the pilot and of `evse_energy_meter` are sampled directly, their
  filters are not used.

The other sensors are outputs: filters on the `platform: evse` sensors and on `total_power`
/ `total_energy` of `evse_energy_meter` only change what is published to Home Assistant
and automations. For example the total energy in kWh:

```yaml
    total_energy:
      name: Total energy
      unit_of_measurement: kWh
      accuracy_decimals: 3
      filters:
        - multiply: 0.001
```

To publish less often, use filters (e.g. `throttle_average: 10s` for the power, `throttle:
10s` for the total energy) instead of a longer `update_interval` of the energy meter,
which is also the interval of the energy integration. To publish the current and voltage
less often without slowing down the EVSE, keep the referenced sensors internal (no
`name`) and publish [`copy`](https://esphome.io/components/sensor/copy/) sensors with
a `throttle_average` filter.

Inputs that the EVSE reads from other entities are different:

* `temperature_sensors`: the EVSE uses the filtered value. A smoothing filter delays the
  over temperature error, a filter that drops values may cause `temperature_fault` after
  `temperature_timeout`.
* `authorization`: the EVSE uses the filtered state of the binary sensor (e.g.
  `delayed_on`).
* The `adc` sensors of the pilot and the energy meter are sampled directly, their filters
  are not used.

### States

| State | Meaning | Pilot | Relay | Charging LED | Error LED |
|---|---|---|---|---|---|
| A | no vehicle | +12 V | off | off | off |
| B1 | connected, not allowed to charge | +12 V | off | blink 500/500 ms | off |
| B2 | connected, ready | PWM | off | blink 500/500 ms | off |
| C1 / D1 | vehicle wants to charge, not allowed | +12 V | off after 6 s | blink 1900/100 ms | off |
| C2 / D2 | charging | PWM | on | on | off |
| E | error | −12 V | off | off | on |
| F | not available | −12 V | off | off | blink 500/500 ms |

Charging is allowed when the EVSE is enabled, available, authorized, no limit is reached
and the RCM self test passed. Pilot fault, diode short and RCM errors clear after 60 s
or with the `clear_error` button, temperature errors when the temperature is back to
normal.

The `Evse` object can also be used from lambdas, e.g.
`id(evse_controller).authorize();`, `id(evse_controller).clear_error();`,
`id(evse_controller).set_charging_current(10.5);`, `id(evse_controller).get_state()`.

### Differences from esp32-evse

The state machine (`components/evse/evse_core.*`) is a line by line port of
`evse.c`, it is unit tested on the host with the esp32-evse tests and more
(`tests/host`). Deliberate differences:

* After all errors clear, the state machine restarts from state A. esp32-evse resumed the
  state before the error when a temperature error cleared, e.g. switching the relay on in
  C2 without checking the pilot, the authorization or the session.
* The RCM self test does not block the main loop; charging waits until it passes.
* Lowering the maximum charging current also lowers the charging current and the default
  charging current.
* The charging current resolution of the PWM is 0.1 A (esp32-evse truncated to 1 A).
* Out of range charging currents are limited instead of rejected.
* The authorization source is level and edge sensitive (see `authorization`).
* Without an energy meter the consumption and under power limits are ignored.
* `enable` / `available` are restored after reboot by default.
* The EVSE takes the current and voltage from sensors, any energy meter can be used.
* The energy meter measures every second (esp32-evse: every 50 ms) and integrates the
  energy with the trapezoidal rule, so the main loop is blocked much less. It measures
  the voltages continuously, the currents while the relay is on.

## `evse_energy_meter` component

The current and voltage of each phase are standard ESPHome sensors, so they take the usual
options (`name`, `filters`, ...) and the `evse` references them by id. Their raw value is
the true RMS voltage at the ADC input in mV (DC offset removed); a filter converts it to A
or V, the `scale` of the esp32-evse `board.yaml` is a `multiply` filter.

```yaml
evse_energy_meter:
  current_l1:
    id: current_l1
    name: Current L1
    adc_sensor: adc_current_l1
    filters:
      - multiply: 0.090909091
  current_l2:
    id: current_l2
    adc_sensor: adc_current_l2
    filters:
      - multiply: 0.090909091
  current_l3:
    id: current_l3
    adc_sensor: adc_current_l3
    filters:
      - multiply: 0.090909091
  voltage_l1:
    id: voltage_l1
    name: Voltage L1
    adc_sensor: adc_voltage_l1
    filters:
      - multiply: 0.47
  voltage_l2:
    id: voltage_l2
    adc_sensor: adc_voltage_l2
    filters:
      - multiply: 0.47
  voltage_l3:
    id: voltage_l3
    adc_sensor: adc_voltage_l3
    filters:
      - multiply: 0.47
  total_power:
    name: Power
  total_energy:
    name: Total energy
  monitored_relay: ac_relay  # optional
```

The phases follow from the configuration: `current_l1` and `voltage_l1` only is a single
phase meter, all of `current_l1` … `current_l3` and `voltage_l1` … `voltage_l3` a three
phase meter, other combinations are rejected.

| Option | Default | Description |
|---|---|---|
| `current_l1` … `current_l3` | `current_l1` required | Current sensors. `adc_sensor`: the `adc` sensor of the input (`attenuation: 12db`, `update_interval: never`, no name). A configuration warning reminds you when there are no `filters`. |
| `voltage_l1` … `voltage_l3` | `voltage_l1` required | Voltage sensors, like the currents. |
| `total_power` | – | Apparent power in W, the sum of V × I of the phases calculated from the filtered values. |
| `total_energy` | – | Energy in Wh, integrated from the total power with the trapezoidal rule, persistent (written to flash at most once per [`flash_write_interval`](https://esphome.io/components/preferences/)). |
| `monitored_relay` | – | Optional switch of the AC relay. The currents are measured only while it is on, otherwise they are 0, so the noise of the current inputs is not counted as energy (esp32-evse measured only while charging). Alternatively remove the noise with a filter of the current sensors, e.g. `- lambda: "return x < 0.5 ? 0.0f : x;"` after the `multiply`. |
| `sample_duration` | `40ms` | RMS window, a whole number of mains periods: 40 ms at 50 Hz, 33 ms or 50 ms at 60 Hz. The inputs are sampled in a tight loop for this time (blocking, like esp32-evse). |
| `update_interval` | `1s` | Measurement interval. |

**Button** (`platform: evse_energy_meter`): `reset_total_energy` (config entity category,
disabled by default).

**Continuing the esp32-evse counter:** Note the total energy you had previously in esp32-evse. From `set_total_energy()` sets the total energy in Wh (persistent). To set it once when migrating, 
e.g. to the total consumption (Wh) shown by esp32-evse, use a `number` component to enter the previous value manually and `button` to write it:

```yaml
number:
  - platform: template
    name: Energy counter set
    optimistic: true
    entity_category: "diagnostic"
    disabled_by_default: true
    id: evse_write_wh
    min_value: 0
    max_value: 100000000
    step: 1
    mode: box

button:
  - platform: template
    name: Energy counter overwrite
    entity_category: "diagnostic"
    disabled_by_default: true
    on_press:
      - lambda: id(energy_meter).set_total_energy(id(evse_write_wh).state);  // Wh
```

After writing, remove/comment out these from the config, so no accidental overwrite occurs in the future.

## Development

Host unit tests of the state machine (g++ with sanitizers), including the tests of
esp32-evse:

```bash
tests/host/run.sh
```

End to end simulation: the complete ESPHome firmware (both components, lights,
numbers, switches, sensors) runs on the ESPHome `host` platform with a simulated
vehicle and 3 phase ADC signals ([`tests/sim`](tests/sim)). The scenario connects,
authorizes, charges at 32 A and 10 A, disables charging (the vehicle ignores it, the
relay opens after 6 s), charges again, disconnects and overheats; the log is checked:

```bash
tests/sim/run.sh
```

Validate / build the example with local components (copy
[`secrets.example.yaml`](secrets.example.yaml) to `secrets.yaml` first):

```bash
sed 's|- source: github://nagyrobi/esp32evse-esphome-fw@main|- source: {type: local, path: components}|' esp32s2da.yaml > test.yaml
esphome compile test.yaml
```

CI runs the unit tests and builds the example with the stable and the dev
ESPHome release, weekly as well, to catch breaking ESPHome changes early.

## Safety

This firmware controls mains voltage. The ESPHome configuration is part of the safety
of the charger: keep the relay switch internal and `ALWAYS_OFF`, check the pilot levels
with the diagnostic sensors, test the RCM and verify the state machine with an EV
simulator before charging a vehicle. Use at your own risk.

## License

GPL-3.0, see [LICENSE](LICENSE). The state machine, pilot, RCM, LED and energy meter
logic is ported from [esp32-evse](https://github.com/dzurikmiroslav/esp32-evse),
which is licensed under GPL-3.0.
