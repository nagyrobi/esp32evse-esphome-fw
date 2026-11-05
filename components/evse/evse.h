#pragma once

// ESPHome EVSE controller, re-implementation of esp32-evse
// (https://github.com/dzurikmiroslav/esp32-evse).
//
// The state machine itself lives in evse_core.* (hardware independent, unit
// tested on the host). This component implements its hardware abstraction:
//  * pilot: ledc output (1kHz PWM / +-12V) + adc sensor via VoltageSampler
//  * AC relay: an ESPHome switch
//  * RCM: sense pin (rising edge interrupt) + test pin
//  * LEDs: ESPHome lights
//  * authorization: binary sensor or switch
//  * temperature: ESPHome sensors
//  * energy: evse_energy_meter component
// and exposes everything as ESPHome entities.

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"
#ifdef USE_LIGHT
#include "esphome/components/light/light_state.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_EVSE_ENERGY_METER
#include "esphome/components/evse_energy_meter/evse_energy_meter.h"
#endif

#include "evse_core.h"

namespace esphome {
namespace evse {

class Evse;

enum EvseNumberType : uint8_t {
  NUMBER_CHARGING_CURRENT = 0,
  NUMBER_DEFAULT_CHARGING_CURRENT,
  NUMBER_MAXIMUM_CHARGING_CURRENT,
  NUMBER_CONSUMPTION_LIMIT,
  NUMBER_DEFAULT_CONSUMPTION_LIMIT,
  NUMBER_CHARGING_TIME_LIMIT,
  NUMBER_DEFAULT_CHARGING_TIME_LIMIT,
  NUMBER_UNDER_POWER_LIMIT,
  NUMBER_DEFAULT_UNDER_POWER_LIMIT,
  NUMBER_TEMPERATURE_THRESHOLD,
  NUMBER_TYPE_COUNT,
};

enum EvseSwitchType : uint8_t {
  SWITCH_ENABLE = 0,
  SWITCH_AVAILABLE,
  SWITCH_TYPE_COUNT,
};

enum EvseBinarySensorType : uint8_t {
  BINARY_SENSOR_PILOT_FAULT = 0,
  BINARY_SENSOR_DIODE_SHORT,
  BINARY_SENSOR_RCM_TRIGGERED,
  BINARY_SENSOR_RCM_SELFTEST_FAULT,
  BINARY_SENSOR_TEMPERATURE_HIGH,
  BINARY_SENSOR_TEMPERATURE_FAULT,
  BINARY_SENSOR_ERROR,
  BINARY_SENSOR_LIMIT_REACHED,
  BINARY_SENSOR_PENDING_AUTHORIZATION,
  BINARY_SENSOR_VEHICLE_CONNECTED,
  BINARY_SENSOR_CHARGING,
  BINARY_SENSOR_TYPE_COUNT,
};

enum EvseSensorType : uint8_t {
  SENSOR_SESSION_ENERGY = 0,
  SENSOR_SESSION_TIME,
  SENSOR_CHARGING_TIME,
  SENSOR_PILOT_HIGH_VOLTAGE,
  SENSOR_PILOT_LOW_VOLTAGE,
  SENSOR_TYPE_COUNT,
};

enum EvseTextSensorType : uint8_t {
  TEXT_SENSOR_STATE = 0,
  TEXT_SENSOR_ERRORS,
  TEXT_SENSOR_TYPE_COUNT,
};

#ifdef USE_NUMBER
class EvseNumber : public number::Number, public Parented<Evse> {
 public:
  void set_type(EvseNumberType type) { this->type_ = type; }

 protected:
  void control(float value) override;
  EvseNumberType type_{NUMBER_CHARGING_CURRENT};
};
#endif

class EvseSwitch : public switch_::Switch, public Parented<Evse> {
 public:
  void set_type(EvseSwitchType type) { this->type_ = type; }

 protected:
  void write_state(bool state) override;
  EvseSwitchType type_{SWITCH_ENABLE};
};

/// Residual current monitor: sense input with rising edge interrupt and a test output
class Rcm {
 public:
  void setup(InternalGPIOPin *pin, GPIOPin *test_pin);
  /// Leakage detected since the last call (edge confirmed by the level after 1ms)
  bool is_triggered();
  void selftest_start();
  RcmSelftestResult selftest_poll();
  void selftest_cancel();

  static void gpio_intr(Rcm *arg);

 protected:
  InternalGPIOPin *pin_{nullptr};
  GPIOPin *test_pin_{nullptr};
  volatile bool triggered_{false};
  bool testing_{false};
  uint32_t test_start_{0};
};

#ifdef USE_LIGHT
/// Drives a light with a blink pattern, like led.c of esp32-evse
class EvseLed {
 public:
  void set_light(light::LightState *light) { this->light_ = light; }
  bool has_light() const { return this->light_ != nullptr; }
  /// on_ms = 0: off, off_ms = 0: on, otherwise blinking
  void set_pattern(uint16_t on_ms, uint16_t off_ms);
  void loop(uint32_t now);

 protected:
  void write_(bool on, bool publish);

  light::LightState *light_{nullptr};
  uint16_t on_ms_{0};
  uint16_t off_ms_{0};
  bool pattern_set_{false};
  bool on_{false};
  uint32_t last_toggle_{0};
};
#endif

class Evse : public PollingComponent, public EvseHal {
 public:
  Evse() : core_(this) {}

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  // --- configuration ---
  void set_pilot_output(output::FloatOutput *output) { this->pilot_output_ = output; }
  void set_pilot_sensor(voltage_sampler::VoltageSampler *sensor) { this->pilot_sensor_ = sensor; }
  void set_pilot_levels(uint16_t level_12, uint16_t level_9, uint16_t level_6, uint16_t level_3, uint16_t level_n12) {
    this->pilot_levels_[0] = level_12;
    this->pilot_levels_[1] = level_9;
    this->pilot_levels_[2] = level_6;
    this->pilot_levels_[3] = level_3;
    this->pilot_levels_[4] = level_n12;
  }
  void set_relay(switch_::Switch *relay) { this->relay_ = relay; }
  void set_rcm_pins(InternalGPIOPin *pin, GPIOPin *test_pin) {
    this->rcm_pin_ = pin;
    this->rcm_test_pin_ = test_pin;
  }
#ifdef USE_LIGHT
  void set_charging_led(light::LightState *light) { this->charging_led_.set_light(light); }
  void set_error_led(light::LightState *light) { this->error_led_.set_light(light); }
  void set_wifi_led(light::LightState *light) { this->wifi_led_.set_light(light); }
#endif
#ifdef USE_BINARY_SENSOR
  void set_authorization_binary_sensor(binary_sensor::BinarySensor *sensor) { this->auth_binary_sensor_ = sensor; }
#endif
  void set_authorization_switch(switch_::Switch *sw) { this->auth_switch_ = sw; }
  void set_authorization_timeout(uint32_t timeout) { this->auth_timeout_ = timeout; }
#ifdef USE_EVSE_ENERGY_METER
  void set_energy_meter(evse_energy_meter::EvseEnergyMeter *meter) { this->energy_meter_ = meter; }
#endif
#ifdef USE_SENSOR
  void add_temperature_sensor(sensor::Sensor *sensor);
#endif
  void set_temperature_timeout(uint32_t timeout) { this->temperature_timeout_ = timeout; }

  // defaults of the settings (used when not changed by the number entities)
  void set_max_charging_current(uint8_t current) { this->max_charging_current_ = current; }
  void set_default_charging_current(float current) { this->default_charging_current_ = current; }
  void set_default_consumption_limit(uint32_t limit) { this->default_consumption_limit_ = limit; }
  void set_default_charging_time_limit(uint32_t limit) { this->default_charging_time_limit_ = limit; }
  void set_default_under_power_limit(uint32_t limit) { this->default_under_power_limit_ = limit; }
  void set_temperature_threshold(uint8_t threshold) { this->temperature_threshold_ = threshold; }

  // --- entities ---
#ifdef USE_NUMBER
  void set_number(EvseNumberType type, EvseNumber *number) { this->numbers_[type] = number; }
  void on_number_control(EvseNumberType type, float value);
#endif
  void set_switch(EvseSwitchType type, EvseSwitch *sw) { this->switches_[type] = sw; }
  void on_switch_control(EvseSwitchType type, bool state);
#ifdef USE_BINARY_SENSOR
  void set_binary_sensor(EvseBinarySensorType type, binary_sensor::BinarySensor *sensor) {
    this->binary_sensors_[type] = sensor;
  }
#endif
#ifdef USE_SENSOR
  void set_sensor(EvseSensorType type, sensor::Sensor *sensor) { this->sensors_[type] = sensor; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_text_sensor(EvseTextSensorType type, text_sensor::TextSensor *sensor) { this->text_sensors_[type] = sensor; }
#endif

  // --- runtime API (also usable from lambdas) ---
  EvseCore &core() { return this->core_; }
  EvseState get_state() const { return this->core_.get_state(); }
  uint32_t get_error() const { return this->core_.get_error(); }
  void authorize() { this->core_.authorize(); }
  void set_enabled(bool enabled);
  void set_available(bool available);
  /// Charging current in A, clamped to 6A .. maximum charging current
  void set_charging_current(float current);
  float get_charging_current() const { return this->core_.get_charging_current() / 10.0f; }

  // --- EvseHal ---
  uint64_t now_ms() override;
  void pilot_measure(PilotVoltage *up_voltage, bool *down_voltage_n12) override;
  void pilot_set_level(bool level) override;
  void pilot_set_amps(uint16_t amps_x10) override;
  void ac_relay_set_state(bool state) override;
  bool rcm_is_triggered() override;
  void rcm_selftest_start() override;
  RcmSelftestResult rcm_selftest_poll() override;
  void rcm_selftest_cancel() override;
  bool temperature_get_high(float *high) override;
  bool temperature_is_error() override;
  bool auth_is_active() override;
  bool energy_meter_is_available() override;
  void energy_meter_start_session() override;
  void energy_meter_stop_session() override;
  void energy_meter_process(bool charging, uint16_t charging_current_x10) override;
  uint32_t energy_meter_get_consumption() override;
  uint32_t energy_meter_get_charging_time() override;
  uint32_t energy_meter_get_power() override;

 protected:
  void process_();
  void restore_settings_();
  void publish_state_();
  void publish_session_();
  void update_leds_();
  void update_wifi_led_();
  void write_relay_(bool state);
#ifdef USE_NUMBER
  void publish_number_(EvseNumberType type, float value);
  void save_number_(EvseNumberType type, float value);
#endif
  float get_session_energy_() const;
  uint32_t get_session_time_();

  EvseCore core_;

  // pilot
  output::FloatOutput *pilot_output_{nullptr};
  voltage_sampler::VoltageSampler *pilot_sensor_{nullptr};
  uint16_t pilot_levels_[5]{};
  float pilot_high_mv_{NAN};
  float pilot_low_mv_{NAN};
  uint32_t pilot_measure_time_{0};  // us

  // relay
  switch_::Switch *relay_{nullptr};
  bool relay_state_{false};

  // rcm
  InternalGPIOPin *rcm_pin_{nullptr};
  GPIOPin *rcm_test_pin_{nullptr};
  Rcm rcm_;

#ifdef USE_LIGHT
  EvseLed charging_led_;
  EvseLed error_led_;
  EvseLed wifi_led_;
#endif
  bool led_state_valid_{false};
  EvseState led_state_{EvseState::A};

  // authorization
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *auth_binary_sensor_{nullptr};
#endif
  switch_::Switch *auth_switch_{nullptr};
  uint32_t auth_timeout_{AUTHORIZED_TIME};

  // temperature
  struct TemperatureSensor {
#ifdef USE_SENSOR
    sensor::Sensor *sensor;
#endif
    float value;
    uint32_t last_valid;
    bool has_value;
  };
  std::vector<TemperatureSensor> temperature_sensors_;
  uint32_t temperature_timeout_{120000};
  uint32_t setup_time_{0};

  // energy meter and session
#ifdef USE_EVSE_ENERGY_METER
  evse_energy_meter::EvseEnergyMeter *energy_meter_{nullptr};
#endif
  bool has_session_{false};
  uint64_t session_start_{0};
  uint64_t charging_time_{0};  // ms
  uint64_t last_process_time_{0};
  double session_energy_{0.0};  // Wh
  double last_total_energy_{0.0};

  // settings
  uint8_t max_charging_current_{32};
  float default_charging_current_{NAN};
  uint32_t default_consumption_limit_{0};
  uint32_t default_charging_time_limit_{0};
  uint32_t default_under_power_limit_{0};
  uint8_t temperature_threshold_{60};

  // 64 bit millis
  uint32_t last_millis_{0};
  uint64_t millis_high_{0};

  // entities
#ifdef USE_NUMBER
  EvseNumber *numbers_[NUMBER_TYPE_COUNT]{};
  ESPPreferenceObject number_prefs_[NUMBER_TYPE_COUNT];
#endif
  EvseSwitch *switches_[SWITCH_TYPE_COUNT]{};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *binary_sensors_[BINARY_SENSOR_TYPE_COUNT]{};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *sensors_[SENSOR_TYPE_COUNT]{};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *text_sensors_[TEXT_SENSOR_TYPE_COUNT]{};
#endif
  // last published values (to publish only changes)
  bool published_valid_{false};
  EvseState published_state_{EvseState::A};
  uint32_t published_error_{0};
  uint8_t published_flags_{0};
};

}  // namespace evse
}  // namespace esphome
