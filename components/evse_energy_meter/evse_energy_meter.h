#pragma once

// Energy meter of the EVSE, port of components/peripherals/src/energy_meter.c from
// esp32-evse (https://github.com/dzurikmiroslav/esp32-evse).
//
// The current and voltage of each phase are standard ESPHome sensors. Their raw value is
// the true RMS voltage at the ADC input in mV, sampled through ESPHome `adc` sensors
// (VoltageSampler interface), with the DC offset removed. Sensor filters convert it to A or
// V: the `scale` of esp32-evse board.yaml is a `multiply` filter.
//
// Total power (apparent power, the sum of V * I of the phases) and total energy are
// calculated from the filtered current and voltage values.
//
// Single phase: current_l1 + voltage_l1, three phase: current_l1..l3 + voltage_l1..l3.
// With a relay switch configured the currents are measured only while it is on, otherwise
// they are 0 (like esp32-evse, which measured only while charging), so the noise of the
// current sensors is not counted.

#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
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
  void set_current(uint8_t phase, sensor::Sensor *sensor, voltage_sampler::VoltageSampler *input);
  void set_voltage(uint8_t phase, sensor::Sensor *sensor, voltage_sampler::VoltageSampler *input);
  void set_total_power_sensor(sensor::Sensor *sensor) { this->total_power_sensor_ = sensor; }
  void set_total_energy_sensor(sensor::Sensor *sensor) { this->total_energy_sensor_ = sensor; }
#ifdef USE_SWITCH
  void set_relay(switch_::Switch *relay) { this->relay_ = relay; }
#endif
  void set_sample_duration(uint32_t sample_duration_ms) { this->sample_duration_us_ = sample_duration_ms * 1000; }

  /// Total (apparent) power in W, NAN when unknown
  float get_total_power() const { return this->power_valid_ ? this->power_ : NAN; }
  /// Total energy in Wh, persistent
  double get_total_energy() const { return this->total_energy_; }
  /// Set the total energy to 0
  void reset_total_energy();

 protected:
  struct Channel {
    sensor::Sensor *sensor{nullptr};
    voltage_sampler::VoltageSampler *input{nullptr};
    float zero{0.0f};  // DC offset in mV, keeps the RMS accumulation centered
  };

  bool relay_closed_() const;
  void on_relay_(bool closed);
  /// Sample the inputs for sample_duration and publish the raw RMS values (mV)
  void measure_(bool currents);
  void publish_currents_zero_();
  /// Total power from the filtered values of the current and voltage sensors
  bool calculate_power_(float *power) const;
  /// Add the energy of the given (average) power since the last integration
  void integrate_(uint32_t now, float power);
  void publish_totals_();
  void save_total_energy_();

  Channel currents_[PHASES];
  Channel voltages_[PHASES];
  uint8_t phases_{0};
  sensor::Sensor *total_power_sensor_{nullptr};
  sensor::Sensor *total_energy_sensor_{nullptr};
#ifdef USE_SWITCH
  switch_::Switch *relay_{nullptr};
#endif
  uint32_t sample_duration_us_{40000};

  float power_{0.0f};
  bool power_valid_{false};
  double total_energy_{0.0};  // Wh
  double saved_total_energy_{0.0};
  uint32_t last_integration_{0};
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
