#pragma once

// Energy meter of the EVSE, port of components/peripherals/src/energy_meter.c
// from esp32-evse (https://github.com/dzurikmiroslav/esp32-evse).
//
// Current and (optionally) voltage are measured as true RMS from ADC samples
// taken through ESPHome `adc` sensors (VoltageSampler interface). The scale
// factors have the same meaning as in the esp32-evse board.yaml: they convert
// the RMS value in mV at the ADC input to A or V.
//
// Modes (selected by the configuration):
//  * no current inputs:        estimated from the charging current set by the EVSE and ac_voltage ("dummy")
//  * current inputs only:      measured current, voltage = ac_voltage
//  * current + voltage inputs: measured current and voltage
//
// Power is the apparent power (sum of Vrms * Irms of the phases), as in esp32-evse.
// The meter only measures while the EVSE is charging (relay closed).

#include <cstdint>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

namespace esphome {
namespace evse_energy_meter {

static constexpr uint8_t PHASES = 3;

class EvseEnergyMeter : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // configuration
  void set_three_phase(bool three_phase) { this->three_phase_ = three_phase; }
  void set_ac_voltage(float ac_voltage) { this->ac_voltage_ = ac_voltage; }
  void add_current_input(voltage_sampler::VoltageSampler *input) { this->current_inputs_.push_back(input); }
  void add_voltage_input(voltage_sampler::VoltageSampler *input) { this->voltage_inputs_.push_back(input); }
  void set_current_scale(float scale) { this->current_scale_ = scale; }
  void set_voltage_scale(float scale) { this->voltage_scale_ = scale; }
  void set_sample_duration(uint32_t sample_duration_ms) { this->sample_duration_us_ = sample_duration_ms * 1000; }

#ifdef USE_SENSOR
  void set_voltage_sensor(uint8_t phase, sensor::Sensor *sensor) { this->voltage_sensors_[phase] = sensor; }
  void set_current_sensor(uint8_t phase, sensor::Sensor *sensor) { this->current_sensors_[phase] = sensor; }
  void set_power_sensor(sensor::Sensor *sensor) { this->power_sensor_ = sensor; }
  void set_total_energy_sensor(sensor::Sensor *sensor) { this->total_energy_sensor_ = sensor; }
#endif

  // interface used by the EVSE
  /// Called by the EVSE controller, the meter measures only while charging
  void set_charging(bool charging, float charging_current);
  /// Actual (apparent) power in W, 0 when not charging
  float get_power() const { return this->power_; }
  /// Total energy in Wh, persistent
  double get_total_energy() const { return this->total_energy_; }
  float get_voltage(uint8_t phase) const { return this->voltage_[phase]; }
  float get_current(uint8_t phase) const { return this->current_[phase]; }

  /// Set the total energy to 0
  void reset_total_energy();

 protected:
  void measure_();
  /// add the energy of the given (average) power since the last integration
  void integrate_(uint32_t now, float power);
  void publish_();
  void save_total_energy_();

  // configuration
  bool three_phase_{false};
  float ac_voltage_{230.0f};
  std::vector<voltage_sampler::VoltageSampler *> current_inputs_;
  std::vector<voltage_sampler::VoltageSampler *> voltage_inputs_;
  float current_scale_{1.0f};
  float voltage_scale_{1.0f};
  uint32_t sample_duration_us_{40000};

#ifdef USE_SENSOR
  sensor::Sensor *voltage_sensors_[PHASES]{};
  sensor::Sensor *current_sensors_[PHASES]{};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *total_energy_sensor_{nullptr};
#endif

  // state
  bool charging_{false};
  float charging_current_{0.0f};
  float voltage_[PHASES]{};
  float current_[PHASES]{};
  float power_{0.0f};
  bool power_valid_{false};   // power_ was measured in this charging period
  double total_energy_{0.0};  // Wh
  double saved_total_energy_{0.0};
  uint32_t last_integration_{0};
  bool idle_published_{false};
  // DC offset (zero) of each input in mV, tracked to keep the RMS accumulation centered
  float current_zero_[PHASES]{};
  float voltage_zero_[PHASES]{};
  ESPPreferenceObject pref_;
};

#ifdef USE_BUTTON
class ResetTotalEnergyButton : public button::Button, public Parented<EvseEnergyMeter> {
 protected:
  void press_action() override { this->parent_->reset_total_energy(); }
};
#endif

}  // namespace evse_energy_meter
}  // namespace esphome
