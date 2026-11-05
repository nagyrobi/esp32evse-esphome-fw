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

void EvseEnergyMeter::setup() {
  this->pref_ = global_preferences->make_preference<double>(TOTAL_ENERGY_PREF_KEY, true);
  double total;
  if (this->pref_.load(&total) && std::isfinite(total) && total >= 0.0) {
    this->total_energy_ = total;
    this->saved_total_energy_ = total;
  }
  ESP_LOGD(TAG, "Total energy %.0fWh", this->total_energy_);
}

void EvseEnergyMeter::dump_config() {
  const char *mode = "dummy (estimated from charging current)";
  if (!this->current_inputs_.empty()) {
    mode = this->voltage_inputs_.empty() ? "current" : "current and voltage";
  }
  ESP_LOGCONFIG(TAG,
                "EVSE energy meter:\n"
                "  Mode: %s\n"
                "  Phases: %u\n"
                "  Current inputs: %u (scale %f)\n"
                "  Voltage inputs: %u (scale %f)\n"
                "  AC voltage: %.0fV\n"
                "  Sample duration: %" PRIu32 "ms\n"
                "  Total energy: %.0fWh",
                mode, this->three_phase_ ? 3u : 1u, (unsigned) this->current_inputs_.size(), this->current_scale_,
                (unsigned) this->voltage_inputs_.size(), this->voltage_scale_, this->ac_voltage_,
                this->sample_duration_us_ / 1000, this->total_energy_);
  LOG_UPDATE_INTERVAL(this);
#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Voltage L1", this->voltage_sensors_[0]);
  LOG_SENSOR("  ", "Voltage L2", this->voltage_sensors_[1]);
  LOG_SENSOR("  ", "Voltage L3", this->voltage_sensors_[2]);
  LOG_SENSOR("  ", "Current L1", this->current_sensors_[0]);
  LOG_SENSOR("  ", "Current L2", this->current_sensors_[1]);
  LOG_SENSOR("  ", "Current L3", this->current_sensors_[2]);
  LOG_SENSOR("  ", "Power", this->power_sensor_);
  LOG_SENSOR("  ", "Total energy", this->total_energy_sensor_);
#endif
}

void EvseEnergyMeter::set_charging(bool charging, float charging_current) {
  this->charging_current_ = charging_current;

  if (charging == this->charging_)
    return;

  uint32_t now = millis();
  if (charging) {
    ESP_LOGD(TAG, "Charging started");
    this->charging_ = true;
    this->idle_published_ = false;
    this->last_integration_ = now;
    this->power_ = 0.0f;
    this->power_valid_ = false;
    // first measurement soon, don't wait for the next update interval
    this->set_timeout("first_measure", 200, [this]() { this->update(); });
  } else {
    ESP_LOGD(TAG, "Charging stopped");
    // the last measured power was flowing until now
    this->integrate_(now, this->power_);
    this->charging_ = false;
    this->power_valid_ = false;
    this->cancel_timeout("first_measure");
    for (uint8_t i = 0; i < PHASES; i++) {
      this->voltage_[i] = 0.0f;
      this->current_[i] = 0.0f;
    }
    this->power_ = 0.0f;
    this->publish_();
    this->save_total_energy_();
    this->idle_published_ = true;
  }
}

void EvseEnergyMeter::update() {
  if (!this->charging_) {
    if (!this->idle_published_) {
      this->publish_();
      this->idle_published_ = true;
    }
    return;
  }

  const float previous_power = this->power_;
  const bool previous_valid = this->power_valid_;
  this->measure_();
  // trapezoidal integration between two measurements
  this->integrate_(millis(), previous_valid ? (previous_power + this->power_) / 2.0f : this->power_);
  this->power_valid_ = true;
  this->publish_();
  this->save_total_energy_();
}

void EvseEnergyMeter::measure_() {
  const uint8_t phases = this->three_phase_ ? PHASES : 1;
  const uint8_t n_cur = std::min<size_t>(this->current_inputs_.size(), phases);
  const uint8_t n_vlt = std::min<size_t>(this->voltage_inputs_.size(), phases);

  if (n_cur > 0 || n_vlt > 0) {
    // Values are centered by the zero (DC offset) of the previous measurement and
    // accumulated in double, so the variance is not lost in float cancellation.
    double cur_sum[PHASES] = {}, cur_sum2[PHASES] = {};
    double vlt_sum[PHASES] = {}, vlt_sum2[PHASES] = {};
    uint32_t cur_count[PHASES] = {}, vlt_count[PHASES] = {};

    const uint32_t start = micros();
    do {
      for (uint8_t i = 0; i < n_cur; i++) {
        float sample = this->current_inputs_[i]->sample();
        if (std::isnan(sample))
          continue;
        double x = sample * 1000.0 - this->current_zero_[i];
        cur_sum[i] += x;
        cur_sum2[i] += x * x;
        cur_count[i]++;
      }
      for (uint8_t i = 0; i < n_vlt; i++) {
        float sample = this->voltage_inputs_[i]->sample();
        if (std::isnan(sample))
          continue;
        double x = sample * 1000.0 - this->voltage_zero_[i];
        vlt_sum[i] += x;
        vlt_sum2[i] += x * x;
        vlt_count[i]++;
      }
    } while (micros() - start < this->sample_duration_us_);

    for (uint8_t i = 0; i < n_cur; i++) {
      if (cur_count[i] == 0) {
        this->current_[i] = 0.0f;
        continue;
      }
      double mean = cur_sum[i] / cur_count[i];
      double variance = cur_sum2[i] / cur_count[i] - mean * mean;
      this->current_zero_[i] += mean;
      this->current_[i] = std::sqrt(std::max(variance, 0.0)) * this->current_scale_;
    }
    for (uint8_t i = 0; i < n_vlt; i++) {
      if (vlt_count[i] == 0) {
        this->voltage_[i] = 0.0f;
        continue;
      }
      double mean = vlt_sum[i] / vlt_count[i];
      double variance = vlt_sum2[i] / vlt_count[i] - mean * mean;
      this->voltage_zero_[i] += mean;
      this->voltage_[i] = std::sqrt(std::max(variance, 0.0)) * this->voltage_scale_;
    }

    ESP_LOGV(TAG, "Samples: current %" PRIu32 ", voltage %" PRIu32, cur_count[0], vlt_count[0]);
  }

  for (uint8_t i = 0; i < PHASES; i++) {
    if (i >= phases) {
      this->current_[i] = 0.0f;
      this->voltage_[i] = 0.0f;
      continue;
    }
    if (n_cur == 0) {
      this->current_[i] = this->charging_current_;  // dummy
    } else if (i >= n_cur) {
      this->current_[i] = this->current_[0];  // single current input on three phases
    }
    if (n_vlt == 0) {
      this->voltage_[i] = this->ac_voltage_;
    } else if (i >= n_vlt) {
      this->voltage_[i] = this->voltage_[0];  // single voltage input on three phases
    }
  }

  float power = 0.0f;
  for (uint8_t i = 0; i < PHASES; i++) {
    power += this->voltage_[i] * this->current_[i];
  }
  this->power_ = power;

  ESP_LOGD(TAG, "Voltage %.1f/%.1f/%.1fV, current %.2f/%.2f/%.2fA, power %.0fW", this->voltage_[0], this->voltage_[1],
           this->voltage_[2], this->current_[0], this->current_[1], this->current_[2], this->power_);
}

void EvseEnergyMeter::integrate_(uint32_t now, float power) {
  uint32_t delta = now - this->last_integration_;
  this->last_integration_ = now;
  if (this->charging_) {
    this->total_energy_ += (double) power * delta / 3600000.0;
  }
}

void EvseEnergyMeter::publish_() {
#ifdef USE_SENSOR
  for (uint8_t i = 0; i < PHASES; i++) {
    if (this->voltage_sensors_[i] != nullptr)
      this->voltage_sensors_[i]->publish_state(this->voltage_[i]);
    if (this->current_sensors_[i] != nullptr)
      this->current_sensors_[i]->publish_state(this->current_[i]);
  }
  if (this->power_sensor_ != nullptr)
    this->power_sensor_->publish_state(this->power_);
  if (this->total_energy_sensor_ != nullptr)
    this->total_energy_sensor_->publish_state(this->total_energy_);
#endif
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
#ifdef USE_SENSOR
  if (this->total_energy_sensor_ != nullptr)
    this->total_energy_sensor_->publish_state(this->total_energy_);
#endif
}

}  // namespace evse_energy_meter
}  // namespace esphome
