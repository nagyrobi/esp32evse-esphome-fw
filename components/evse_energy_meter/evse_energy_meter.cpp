#include "evse_energy_meter.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace evse_energy_meter {

static const char *const TAG = "evse_energy_meter";

// Preference key of the total energy
static const uint32_t TOTAL_ENERGY_PREF_KEY = 0x45564D31;  // "EVM1"
// First measurement after the relay closed, don't wait for the next update interval
static constexpr uint32_t FIRST_MEASURE_DELAY = 200;  // ms

void EvseEnergyMeter::set_current(uint8_t phase, sensor::Sensor *sensor, voltage_sampler::VoltageSampler *input) {
  this->currents_[phase].sensor = sensor;
  this->currents_[phase].input = input;
  this->phases_ = std::max<uint8_t>(this->phases_, phase + 1);
}

void EvseEnergyMeter::set_voltage(uint8_t phase, sensor::Sensor *sensor, voltage_sampler::VoltageSampler *input) {
  this->voltages_[phase].sensor = sensor;
  this->voltages_[phase].input = input;
}

void EvseEnergyMeter::setup() {
  this->pref_ = global_preferences->make_preference<double>(TOTAL_ENERGY_PREF_KEY, true);
  double total;
  if (this->pref_.load(&total) && std::isfinite(total) && total >= 0.0) {
    this->total_energy_ = total;
    this->saved_total_energy_ = total;
  }
  this->last_integration_ = millis();

#ifdef USE_SWITCH
  if (this->monitored_relay_ != nullptr) {
    this->monitored_relay_->add_on_state_callback([this](bool state) { this->on_relay_(state); });
  }
#endif
}

void EvseEnergyMeter::dump_config() {
  bool relay = false;
#ifdef USE_SWITCH
  relay = this->monitored_relay_ != nullptr;
#endif
  ESP_LOGCONFIG(TAG,
                "EVSE energy meter:\n"
                "  Phases: %u\n"
                "  Sample duration: %" PRIu32 "ms\n"
                "  Currents only while the monitored relay is on: %s\n"
                "  Total energy: %.0fWh",
                (unsigned) this->phases_, this->sample_duration_us_ / 1000, YESNO(relay), this->total_energy_);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Current L1", this->currents_[0].sensor);
  LOG_SENSOR("  ", "Current L2", this->currents_[1].sensor);
  LOG_SENSOR("  ", "Current L3", this->currents_[2].sensor);
  LOG_SENSOR("  ", "Voltage L1", this->voltages_[0].sensor);
  LOG_SENSOR("  ", "Voltage L2", this->voltages_[1].sensor);
  LOG_SENSOR("  ", "Voltage L3", this->voltages_[2].sensor);
  LOG_SENSOR("  ", "Total power", this->total_power_sensor_);
  LOG_SENSOR("  ", "Total energy", this->total_energy_sensor_);
}

bool EvseEnergyMeter::relay_closed_() const {
#ifdef USE_SWITCH
  if (this->monitored_relay_ != nullptr)
    return this->monitored_relay_->state;
#endif
  return true;
}

void EvseEnergyMeter::on_relay_(bool closed) {
  uint32_t now = millis();
  if (closed) {
    // nothing flowed while the relay was open
    this->last_integration_ = now;
    this->power_valid_ = false;
    this->set_timeout("measure", FIRST_MEASURE_DELAY, [this]() { this->update(); });
  } else {
    // the last measured power was flowing until the relay opened
    if (this->power_valid_)
      this->integrate_(now, this->power_);
    this->last_integration_ = now;
    this->power_ = 0.0f;
    this->power_valid_ = true;
    this->cancel_timeout("measure");
    this->publish_currents_zero_();
    this->publish_totals_();
    this->save_total_energy_();
  }
}

void EvseEnergyMeter::update() {
  const bool relay_closed = this->relay_closed_();

  this->measure_(relay_closed);
  if (!relay_closed)
    this->publish_currents_zero_();

  const uint32_t now = millis();
  float power = 0.0f;
  if (!relay_closed || this->calculate_power_(&power)) {
    // trapezoidal integration between two measurements
    this->integrate_(now, this->power_valid_ ? (this->power_ + power) / 2.0f : power);
    this->power_ = power;
    this->power_valid_ = true;
  } else {
    // a current or voltage is unknown
    this->last_integration_ = now;
    this->power_valid_ = false;
  }

  this->publish_totals_();
  this->save_total_energy_();

  float voltage[PHASES] = {}, current[PHASES] = {};
  for (uint8_t i = 0; i < this->phases_; i++) {
    voltage[i] = this->voltages_[i].sensor->state;
    current[i] = this->currents_[i].sensor->state;
  }
  if (relay_closed) {
    ESP_LOGD(TAG, "Voltage %.1f/%.1f/%.1fV, current %.2f/%.2f/%.2fA, power %.0fW", voltage[0], voltage[1], voltage[2],
             current[0], current[1], current[2], this->get_total_power());
  } else {
    ESP_LOGV(TAG, "Voltage %.1f/%.1f/%.1fV, relay off", voltage[0], voltage[1], voltage[2]);
  }
}

void EvseEnergyMeter::measure_(bool currents) {
  Channel *channels[2 * PHASES];
  uint8_t count = 0;
  for (uint8_t i = 0; i < this->phases_; i++)
    channels[count++] = &this->voltages_[i];
  if (currents) {
    for (uint8_t i = 0; i < this->phases_; i++)
      channels[count++] = &this->currents_[i];
  }

  // Values are centered by the zero (DC offset) of the previous measurement and
  // accumulated in double, so the variance is not lost in float cancellation.
  double sum[2 * PHASES] = {}, sum2[2 * PHASES] = {};
  uint32_t samples[2 * PHASES] = {};

  const uint32_t start = micros();
  do {
    for (uint8_t c = 0; c < count; c++) {
      float sample = channels[c]->input->sample();
      if (std::isnan(sample))
        continue;
      double x = sample * 1000.0 - channels[c]->zero;
      sum[c] += x;
      sum2[c] += x * x;
      samples[c]++;
    }
  } while (micros() - start < this->sample_duration_us_);

  for (uint8_t c = 0; c < count; c++) {
    Channel *channel = channels[c];
    if (samples[c] == 0) {
      channel->sensor->publish_state(NAN);
      continue;
    }
    double mean = sum[c] / samples[c];
    double variance = sum2[c] / samples[c] - mean * mean;
    channel->zero += mean;
    // raw value: RMS in mV at the ADC input, the filters of the sensor convert it to A / V
    channel->sensor->publish_state(std::sqrt(std::max(variance, 0.0)));
  }

  ESP_LOGV(TAG, "Samples per input: %" PRIu32, count > 0 ? samples[0] : 0);
}

void EvseEnergyMeter::publish_currents_zero_() {
  for (uint8_t i = 0; i < this->phases_; i++)
    this->currents_[i].sensor->publish_state(0.0f);
}

bool EvseEnergyMeter::calculate_power_(float *power) const {
  if (this->phases_ == 0)
    return false;
  float sum = 0.0f;
  for (uint8_t i = 0; i < this->phases_; i++) {
    const sensor::Sensor *current = this->currents_[i].sensor;
    const sensor::Sensor *voltage = this->voltages_[i].sensor;
    if (current == nullptr || voltage == nullptr || !current->has_state() || !voltage->has_state() ||
        std::isnan(current->state) || std::isnan(voltage->state))
      return false;
    sum += current->state * voltage->state;
  }
  *power = sum;
  return true;
}

void EvseEnergyMeter::integrate_(uint32_t now, float power) {
  uint32_t delta = now - this->last_integration_;
  this->last_integration_ = now;
  if (power > 0.0f)
    this->total_energy_ += static_cast<double>(power) * delta / 3600000.0;
}

void EvseEnergyMeter::publish_totals_() {
  if (this->total_power_sensor_ != nullptr)
    this->total_power_sensor_->publish_state(this->get_total_power());
  if (this->total_energy_sensor_ != nullptr)
    this->total_energy_sensor_->publish_state(this->total_energy_);
}

void EvseEnergyMeter::save_total_energy_() {
  // ESPHome writes the preferences to flash at most once per flash_write_interval
  if (this->total_energy_ != this->saved_total_energy_) {
    this->pref_.save(&this->total_energy_);
    this->saved_total_energy_ = this->total_energy_;
  }
}

void EvseEnergyMeter::reset_total_energy() {
  ESP_LOGI(TAG, "Reset total energy");
  this->total_energy_ = 0.0;
  this->save_total_energy_();
  this->publish_totals_();
}

}  // namespace evse_energy_meter
}  // namespace esphome
