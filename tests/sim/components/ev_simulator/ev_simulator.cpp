#include "ev_simulator.h"

#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ev_simulator {

static const char *const TAG = "ev_simulator";

// ESP32-S2-DA like pilot feedback: +12V -> 2.55V, -12V -> 0.35V at the ADC
static constexpr float ADC_AT_N12V = 0.35f;
static constexpr float ADC_AT_P12V = 2.55f;
// scale factors of the board (A or V per mV RMS)
static constexpr float CURRENT_SCALE = 0.090909091f;
static constexpr float VOLTAGE_SCALE = 0.47f;
static constexpr float ADC_ZERO_MV = 1250.0f;

void EvSimulator::set_ev_state(char state) {
  ESP_LOGI(TAG, "EV state %c", state);
  this->ev_state_ = state;
}

float EvSimulator::pilot_voltage() const {
  float level = this->pilot_output_->get_level();
  bool high;
  if (level >= 0.9999f) {
    high = true;
  } else if (level <= 0.0001f) {
    high = false;
  } else {
    high = (micros() % 1000) < static_cast<uint32_t>(level * 1000.0f);  // 1kHz PWM
  }
  if (!high)
    return -12.0f;
  switch (this->ev_state_) {
    case 'B':
      return 9.0f;
    case 'C':
      return 6.0f;
    case 'D':
      return 3.0f;
    default:
      return 12.0f;
  }
}

float EvSimulator::ev_current() const {
  if ((this->ev_state_ != 'C' && this->ev_state_ != 'D') || this->relay_ == nullptr || !this->relay_->state)
    return 0.0f;
  float level = this->pilot_output_->get_level();
  if (level <= 0.0001f || level >= 0.9999f)
    return 0.0f;  // no PWM, the EV must not draw current
  float duty = level * 100.0f;
  return duty <= 85.0f ? duty * 0.6f : (duty - 64.0f) * 2.5f;
}

float PilotSampler::sample() {
  float voltage = this->parent_->pilot_voltage();
  return ADC_AT_N12V + (voltage + 12.0f) * (ADC_AT_P12V - ADC_AT_N12V) / 24.0f;
}

float WaveSampler::sample() {
  float rms_mv =
      this->is_current_ ? this->parent_->ev_current() / CURRENT_SCALE : this->parent_->grid_voltage() / VOLTAGE_SCALE;
  float angle = 2.0f * static_cast<float>(M_PI) *
                (static_cast<float>(micros() % 20000) / 20000.0f + this->phase_ / 3.0f);  // 50Hz
  return (ADC_ZERO_MV + rms_mv * static_cast<float>(M_SQRT2) * std::sin(angle)) / 1000.0f;
}

}  // namespace ev_simulator
}  // namespace esphome
