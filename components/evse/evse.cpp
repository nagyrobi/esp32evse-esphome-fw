#include "evse.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <string>

#include "esphome/core/log.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome {
namespace evse {

static const char *const TAG = "evse";

static constexpr uint32_t PROCESS_INTERVAL = 50;       // ms, main loop period of esp32-evse
static constexpr int PILOT_SAMPLES = 250;              // ADC samples per pilot measurement
static constexpr int PILOT_HI_LO_SAMPLES = 3;          // averaged highest / lowest samples
static constexpr uint32_t RCM_SELFTEST_TIMEOUT = 500;  // ms

static bool is_persisted_number(EvseNumberType type) {
  switch (type) {
    case NUMBER_DEFAULT_CHARGING_CURRENT:
    case NUMBER_MAXIMUM_CHARGING_CURRENT:
    case NUMBER_DEFAULT_CONSUMPTION_LIMIT:
    case NUMBER_DEFAULT_CHARGING_TIME_LIMIT:
    case NUMBER_DEFAULT_UNDER_POWER_LIMIT:
    case NUMBER_TEMPERATURE_THRESHOLD:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Entities

#ifdef USE_NUMBER
void EvseNumber::control(float value) { this->parent_->on_number_control(this->type_, value); }
#endif

void EvseSwitch::write_state(bool state) { this->parent_->on_switch_control(this->type_, state); }

#ifdef USE_BUTTON
void ClearErrorButton::press_action() { this->parent_->clear_error(); }
#endif

// ---------------------------------------------------------------------------
// Residual current monitor

void IRAM_ATTR Rcm::gpio_intr(Rcm *arg) { arg->triggered_ = true; }

void Rcm::setup(InternalGPIOPin *pin, GPIOPin *test_pin) {
  this->pin_ = pin;
  this->test_pin_ = test_pin;

  this->test_pin_->setup();
  this->test_pin_->digital_write(false);

  this->pin_->setup();
  this->pin_->attach_interrupt(&Rcm::gpio_intr, this, gpio::INTERRUPT_RISING_EDGE);
}

bool Rcm::is_triggered() {
  if (this->testing_ || !this->triggered_)
    return false;
  this->triggered_ = false;
  delayMicroseconds(1000);  // prevent from noise triggering
  return this->pin_->digital_read();
}

void Rcm::selftest_start() {
  this->triggered_ = false;
  this->test_pin_->digital_write(true);
  this->testing_ = true;
  this->test_start_ = millis();
}

RcmSelftestResult Rcm::selftest_poll() {
  if (!this->testing_)
    return RcmSelftestResult::FAILED;

  if (this->triggered_) {
    this->selftest_cancel();
    return RcmSelftestResult::PASSED;
  }
  if (millis() - this->test_start_ >= RCM_SELFTEST_TIMEOUT) {
    this->selftest_cancel();
    return RcmSelftestResult::FAILED;
  }
  return RcmSelftestResult::PENDING;
}

void Rcm::selftest_cancel() {
  this->test_pin_->digital_write(false);
  this->testing_ = false;
  this->triggered_ = false;
}

// ---------------------------------------------------------------------------
// LEDs

#ifdef USE_LIGHT
void EvseLed::set_pattern(uint16_t on_ms, uint16_t off_ms) {
  if (this->light_ == nullptr)
    return;
  if (this->pattern_set_ && this->on_ms_ == on_ms && this->off_ms_ == off_ms)
    return;
  this->pattern_set_ = true;
  this->on_ms_ = on_ms;
  this->off_ms_ = off_ms;
  this->last_toggle_ = millis();
  // the pattern change is published, the blinking itself is not (no log / API spam, no flash writes)
  this->write_(on_ms > 0, true);
}

void EvseLed::loop(uint32_t now) {
  if (this->light_ == nullptr || this->on_ms_ == 0 || this->off_ms_ == 0)
    return;
  uint32_t period = this->on_ ? this->on_ms_ : this->off_ms_;
  if (now - this->last_toggle_ >= period) {
    this->last_toggle_ = now;
    this->write_(!this->on_, false);
  }
}

void EvseLed::write_(bool on, bool publish) {
  this->on_ = on;
  auto call = this->light_->make_call();
  call.set_state(on);
  call.set_transition_length_if_supported(0);
  call.set_publish(publish);
  call.set_save(false);
  call.perform();
}
#endif

// ---------------------------------------------------------------------------
// EVSE

void Evse::setup() {
  this->setup_time_ = millis();

  this->core_.set_rcm(this->rcm_pin_ != nullptr && this->rcm_test_pin_ != nullptr);
  this->core_.set_temperature_monitoring(!this->temperature_sensors_.empty());
  bool require_auth = this->auth_switch_ != nullptr;
#ifdef USE_BINARY_SENSOR
  require_auth |= this->auth_binary_sensor_ != nullptr;
#endif
  this->core_.set_require_auth(require_auth);
  this->core_.set_auth_timeout(this->auth_timeout_);

  // pilot +12V, relay off
  this->core_.init();

  // duration of a pilot measurement, the samples should span several 1ms PWM periods
  {
    PilotVoltage up_voltage;
    bool down_voltage_n12;
    uint32_t start = micros();
    this->pilot_measure(&up_voltage, &down_voltage_n12);
    this->pilot_measure_time_ = micros() - start;
  }

  if (this->core_.is_rcm()) {
    this->rcm_.setup(this->rcm_pin_, this->rcm_test_pin_);
  }

  this->restore_settings_();

  // switches, apply the restore mode
  for (uint8_t i = 0; i < SWITCH_TYPE_COUNT; i++) {
    if (this->switches_[i] == nullptr)
      continue;
    optional<bool> initial = this->switches_[i]->get_initial_state_with_restore_mode();
    this->on_switch_control(static_cast<EvseSwitchType>(i), initial.has_value() ? *initial : true);
  }

  // authorization: a new activation grants authorization for the authorization timeout
#ifdef USE_BINARY_SENSOR
  if (this->auth_binary_sensor_ != nullptr) {
    this->auth_binary_sensor_->add_on_state_callback([this](bool state) {
      if (state)
        this->core_.authorize();
    });
  }
#endif
  if (this->auth_switch_ != nullptr) {
    this->auth_switch_->add_on_state_callback([this](bool state) {
      if (state)
        this->core_.authorize();
    });
  }

  // the relay is controlled only by the state machine
  this->relay_->add_on_state_callback([this](bool state) {
    if (state != this->relay_state_) {
      ESP_LOGW(TAG, "AC relay was switched outside of the EVSE controller, restoring");
      this->defer([this]() { this->write_relay_(this->relay_state_); });
    }
  });

#ifdef USE_SENSOR
  for (size_t i = 0; i < this->temperature_sensors_.size(); i++) {
    this->temperature_sensors_[i].sensor->add_on_state_callback([this, i](float value) {
      if (std::isnan(value))
        return;
      TemperatureSensor &temp = this->temperature_sensors_[i];
      temp.value = value;
      temp.last_valid = millis();
      temp.has_value = true;
    });
  }
#endif

  this->publish_state_();
  this->publish_session_();

#ifdef USE_LIGHT
  this->update_leds_();
  if (!this->charging_led_.has_light() && !this->error_led_.has_light() && !this->wifi_led_.has_light())
    this->disable_loop();
#else
  this->disable_loop();
#endif

  this->set_interval("process", PROCESS_INTERVAL, [this]() { this->process_(); });
}

void Evse::restore_settings_() {
#ifdef USE_NUMBER
  auto load = [this](EvseNumberType type, float *value) -> bool {
    if (this->numbers_[type] == nullptr || !is_persisted_number(type))
      return false;
    this->number_prefs_[type] = this->numbers_[type]->make_entity_preference<float>();
    float stored;
    if (!this->number_prefs_[type].load(&stored) || !std::isfinite(stored))
      return false;
    *value = stored;
    return true;
  };
#else
  auto load = [](EvseNumberType /*type*/, float * /*value*/) -> bool { return false; };
#endif
  float value;

  // maximum charging current
  if (load(NUMBER_MAXIMUM_CHARGING_CURRENT, &value))
    this->max_charging_current_ = clamp<long>(lroundf(value), MAX_CHARGING_CURRENT_MIN, MAX_CHARGING_CURRENT_MAX);
  this->core_.set_max_charging_current(this->max_charging_current_);

  // default charging current, the charging current after boot
  if (std::isnan(this->default_charging_current_))
    this->default_charging_current_ = this->max_charging_current_;
  if (load(NUMBER_DEFAULT_CHARGING_CURRENT, &value))
    this->default_charging_current_ = value;
  this->default_charging_current_ =
      clamp(roundf(this->default_charging_current_ * 10.0f) / 10.0f, CHARGING_CURRENT_MIN / 10.0f,
            static_cast<float>(this->max_charging_current_));
  this->core_.set_charging_current(lroundf(this->default_charging_current_ * 10.0f));

  // default limits, the limits after boot
  if (load(NUMBER_DEFAULT_CONSUMPTION_LIMIT, &value))
    this->default_consumption_limit_ = std::max(lroundf(value), 0L);
  this->core_.set_consumption_limit(this->default_consumption_limit_);
  if (load(NUMBER_DEFAULT_CHARGING_TIME_LIMIT, &value))
    this->default_charging_time_limit_ = std::max(lroundf(value), 0L);
  this->core_.set_charging_time_limit(this->default_charging_time_limit_);
  if (load(NUMBER_DEFAULT_UNDER_POWER_LIMIT, &value))
    this->default_under_power_limit_ = std::max(lroundf(value), 0L);
  this->core_.set_under_power_limit(this->default_under_power_limit_);

  // temperature threshold
  if (load(NUMBER_TEMPERATURE_THRESHOLD, &value))
    this->temperature_threshold_ = clamp<long>(lroundf(value), TEMP_THRESHOLD_MIN, TEMP_THRESHOLD_MAX);
  this->core_.set_temp_threshold(this->temperature_threshold_);

#ifdef USE_NUMBER
  this->publish_number_(NUMBER_CHARGING_CURRENT, this->get_charging_current());
  this->publish_number_(NUMBER_DEFAULT_CHARGING_CURRENT, this->default_charging_current_);
  this->publish_number_(NUMBER_MAXIMUM_CHARGING_CURRENT, this->max_charging_current_);
  this->publish_number_(NUMBER_CONSUMPTION_LIMIT, this->core_.get_consumption_limit());
  this->publish_number_(NUMBER_DEFAULT_CONSUMPTION_LIMIT, this->default_consumption_limit_);
  this->publish_number_(NUMBER_CHARGING_TIME_LIMIT, this->core_.get_charging_time_limit());
  this->publish_number_(NUMBER_DEFAULT_CHARGING_TIME_LIMIT, this->default_charging_time_limit_);
  this->publish_number_(NUMBER_UNDER_POWER_LIMIT, this->core_.get_under_power_limit());
  this->publish_number_(NUMBER_DEFAULT_UNDER_POWER_LIMIT, this->default_under_power_limit_);
  this->publish_number_(NUMBER_TEMPERATURE_THRESHOLD, this->temperature_threshold_);
#endif
}

void Evse::loop() {
#ifdef USE_LIGHT
  uint32_t now = millis();
  this->update_wifi_led_();
  this->charging_led_.loop(now);
  this->error_led_.loop(now);
  this->wifi_led_.loop(now);
#endif
}

void Evse::update() {
  this->publish_session_();
#ifdef USE_SENSOR
  if (this->sensors_[SENSOR_PILOT_HIGH_VOLTAGE] != nullptr)
    this->sensors_[SENSOR_PILOT_HIGH_VOLTAGE]->publish_state(this->pilot_high_mv_);
  if (this->sensors_[SENSOR_PILOT_LOW_VOLTAGE] != nullptr)
    this->sensors_[SENSOR_PILOT_LOW_VOLTAGE]->publish_state(this->pilot_low_mv_);
#endif
}

void Evse::process_() {
  this->core_.process();
  this->publish_state_();
  this->update_leds_();
}

void Evse::dump_config() {
  bool auth = this->auth_switch_ != nullptr;
#ifdef USE_BINARY_SENSOR
  auth |= this->auth_binary_sensor_ != nullptr;
#endif
  ESP_LOGCONFIG(TAG,
                "EVSE:\n"
                "  Pilot levels: 12V >= %umV, 9V >= %umV, 6V >= %umV, 3V >= %umV, -12V <= %umV\n"
                "  Residual current monitor: %s\n"
                "  Authorization: %s (timeout %" PRIu32 "s)\n"
                "  Temperature sensors: %u (threshold %u°C)\n"
                "  Energy meter: %s\n"
                "  Maximum charging current: %uA\n"
                "  Charging current: %.1fA\n"
                "  Pilot measurement: %d samples in %" PRIu32 "us",
                this->pilot_levels_[0], this->pilot_levels_[1], this->pilot_levels_[2], this->pilot_levels_[3],
                this->pilot_levels_[4], YESNO(this->core_.is_rcm()), YESNO(auth), this->auth_timeout_ / 1000,
                (unsigned) this->temperature_sensors_.size(), this->core_.get_temp_threshold(),
                this->meter_phases_ == 0 ? "no" : (this->meter_phases_ == 1 ? "single phase" : "three phase"),
                this->core_.get_max_charging_current(), this->get_charging_current(), PILOT_SAMPLES,
                this->pilot_measure_time_);
  if (this->core_.is_rcm()) {
    LOG_PIN("  RCM Pin: ", this->rcm_pin_);
    LOG_PIN("  RCM Test Pin: ", this->rcm_test_pin_);
  }
  LOG_UPDATE_INTERVAL(this);
}

// --- runtime API ---

void Evse::set_enabled(bool enabled) { this->on_switch_control(SWITCH_ENABLE, enabled); }

void Evse::set_available(bool available) { this->on_switch_control(SWITCH_AVAILABLE, available); }

void Evse::set_charging_current(float current) {
  if (std::isnan(current))
    return;
  float value =
      clamp(current, CHARGING_CURRENT_MIN / 10.0f, static_cast<float>(this->core_.get_max_charging_current()));
  if (value != current) {
    ESP_LOGW(TAG, "Charging current %.1fA limited to %.1fA", current, value);
  }
  this->core_.set_charging_current(lroundf(value * 10.0f));
#ifdef USE_NUMBER
  this->publish_number_(NUMBER_CHARGING_CURRENT, this->get_charging_current());
#endif
}

void Evse::on_switch_control(EvseSwitchType type, bool state) {
  switch (type) {
    case SWITCH_ENABLE:
      this->core_.set_enabled(state);
      break;
    case SWITCH_AVAILABLE:
      this->core_.set_available(state);
      break;
    default:
      return;
  }
  if (this->switches_[type] != nullptr)
    this->switches_[type]->publish_state(state);
}

#ifdef USE_NUMBER
void Evse::publish_number_(EvseNumberType type, float value) {
  if (this->numbers_[type] != nullptr)
    this->numbers_[type]->publish_state(value);
}

void Evse::save_number_(EvseNumberType type, float value) {
  if (this->numbers_[type] != nullptr && is_persisted_number(type))
    this->number_prefs_[type].save(&value);
}

void Evse::on_number_control(EvseNumberType type, float value) {
  if (std::isnan(value))
    return;

  switch (type) {
    case NUMBER_CHARGING_CURRENT:
      this->set_charging_current(value);
      break;

    case NUMBER_DEFAULT_CHARGING_CURRENT: {
      float current = clamp(roundf(value * 10.0f) / 10.0f, CHARGING_CURRENT_MIN / 10.0f,
                            static_cast<float>(this->core_.get_max_charging_current()));
      this->default_charging_current_ = current;
      this->save_number_(type, current);
      this->publish_number_(type, current);
      break;
    }

    case NUMBER_MAXIMUM_CHARGING_CURRENT: {
      uint8_t current = clamp<long>(lroundf(value), MAX_CHARGING_CURRENT_MIN, MAX_CHARGING_CURRENT_MAX);
      this->max_charging_current_ = current;
      // also lowers the charging current when needed
      this->core_.set_max_charging_current(current);
      this->save_number_(type, current);
      this->publish_number_(type, current);
      if (this->default_charging_current_ > current) {
        this->default_charging_current_ = current;
        this->save_number_(NUMBER_DEFAULT_CHARGING_CURRENT, this->default_charging_current_);
        this->publish_number_(NUMBER_DEFAULT_CHARGING_CURRENT, this->default_charging_current_);
      }
      this->publish_number_(NUMBER_CHARGING_CURRENT, this->get_charging_current());
      break;
    }

    case NUMBER_CONSUMPTION_LIMIT:
      this->core_.set_consumption_limit(std::max(lroundf(value), 0L));
      this->publish_number_(type, this->core_.get_consumption_limit());
      break;

    case NUMBER_DEFAULT_CONSUMPTION_LIMIT:
      this->default_consumption_limit_ = std::max(lroundf(value), 0L);
      this->save_number_(type, this->default_consumption_limit_);
      this->publish_number_(type, this->default_consumption_limit_);
      break;

    case NUMBER_CHARGING_TIME_LIMIT:
      this->core_.set_charging_time_limit(std::max(lroundf(value), 0L));
      this->publish_number_(type, this->core_.get_charging_time_limit());
      break;

    case NUMBER_DEFAULT_CHARGING_TIME_LIMIT:
      this->default_charging_time_limit_ = std::max(lroundf(value), 0L);
      this->save_number_(type, this->default_charging_time_limit_);
      this->publish_number_(type, this->default_charging_time_limit_);
      break;

    case NUMBER_UNDER_POWER_LIMIT:
      this->core_.set_under_power_limit(std::max(lroundf(value), 0L));
      this->publish_number_(type, this->core_.get_under_power_limit());
      break;

    case NUMBER_DEFAULT_UNDER_POWER_LIMIT:
      this->default_under_power_limit_ = std::max(lroundf(value), 0L);
      this->save_number_(type, this->default_under_power_limit_);
      this->publish_number_(type, this->default_under_power_limit_);
      break;

    case NUMBER_TEMPERATURE_THRESHOLD: {
      uint8_t threshold = clamp<long>(lroundf(value), TEMP_THRESHOLD_MIN, TEMP_THRESHOLD_MAX);
      this->temperature_threshold_ = threshold;
      this->core_.set_temp_threshold(threshold);
      this->save_number_(type, threshold);
      this->publish_number_(type, threshold);
      break;
    }

    default:
      break;
  }
}
#endif

// --- publishing ---

void Evse::publish_state_() {
  EvseState state = this->core_.get_state();
  uint32_t error = this->core_.get_error();
  uint8_t flags = (this->core_.is_limit_reached() ? 1 : 0) | (this->core_.is_pending_auth() ? 2 : 0);

  bool force = !this->published_valid_;
  bool state_changed = force || state != this->published_state_;
  bool error_changed = force || error != this->published_error_;
  if (!state_changed && !error_changed && flags == this->published_flags_)
    return;

  this->published_valid_ = true;
  this->published_state_ = state;
  this->published_error_ = error;
  this->published_flags_ = flags;

#ifdef USE_TEXT_SENSOR
  if (state_changed && this->text_sensors_[TEXT_SENSOR_STATE] != nullptr)
    this->text_sensors_[TEXT_SENSOR_STATE]->publish_state(evse_state_to_str(state));

  if (error_changed && this->text_sensors_[TEXT_SENSOR_ERRORS] != nullptr) {
    static const struct {
      uint32_t bit;
      const char *name;
    } ERROR_NAMES[] = {
        {ERR_PILOT_FAULT_BIT, "pilot_fault"},           {ERR_DIODE_SHORT_BIT, "diode_short"},
        {ERR_RCM_TRIGGERED_BIT, "rcm_triggered"},       {ERR_RCM_SELFTEST_FAULT_BIT, "rcm_selftest_fault"},
        {ERR_TEMPERATURE_HIGH_BIT, "temperature_high"}, {ERR_TEMPERATURE_FAULT_BIT, "temperature_fault"},
    };
    std::string errors;
    for (const auto &entry : ERROR_NAMES) {
      if (error & entry.bit) {
        if (!errors.empty())
          errors += ", ";
        errors += entry.name;
      }
    }
    this->text_sensors_[TEXT_SENSOR_ERRORS]->publish_state(errors.empty() ? "none" : errors);
  }
#endif

#ifdef USE_BINARY_SENSOR
  auto publish = [this](EvseBinarySensorType type, bool value) {
    if (this->binary_sensors_[type] != nullptr)
      this->binary_sensors_[type]->publish_state(value);
  };
  publish(BINARY_SENSOR_PILOT_FAULT, error & ERR_PILOT_FAULT_BIT);
  publish(BINARY_SENSOR_DIODE_SHORT, error & ERR_DIODE_SHORT_BIT);
  publish(BINARY_SENSOR_RCM_TRIGGERED, error & ERR_RCM_TRIGGERED_BIT);
  publish(BINARY_SENSOR_RCM_SELFTEST_FAULT, error & ERR_RCM_SELFTEST_FAULT_BIT);
  publish(BINARY_SENSOR_TEMPERATURE_HIGH, error & ERR_TEMPERATURE_HIGH_BIT);
  publish(BINARY_SENSOR_TEMPERATURE_FAULT, error & ERR_TEMPERATURE_FAULT_BIT);
  publish(BINARY_SENSOR_ERROR, error != 0);
  publish(BINARY_SENSOR_LIMIT_REACHED, this->core_.is_limit_reached());
  publish(BINARY_SENSOR_PENDING_AUTHORIZATION, this->core_.is_pending_auth());
  publish(BINARY_SENSOR_VEHICLE_CONNECTED, evse_state_is_session(state));
  publish(BINARY_SENSOR_CHARGING, evse_state_is_charging(state));
#endif

  // session start / end
  if (state_changed)
    this->publish_session_();
}

void Evse::publish_session_() {
#ifdef USE_SENSOR
  if (this->sensors_[SENSOR_SESSION_ENERGY] != nullptr)
    this->sensors_[SENSOR_SESSION_ENERGY]->publish_state(this->get_session_energy_());
  if (this->sensors_[SENSOR_SESSION_TIME] != nullptr)
    this->sensors_[SENSOR_SESSION_TIME]->publish_state(this->get_session_time_());
  if (this->sensors_[SENSOR_CHARGING_TIME] != nullptr)
    this->sensors_[SENSOR_CHARGING_TIME]->publish_state(this->energy_meter_get_charging_time());
#endif
}

float Evse::get_session_energy_() const { return this->meter_phases_ > 0 ? this->session_energy_ : NAN; }

bool Evse::meter_power_(float *power) const {
#ifdef USE_SENSOR
  if (this->meter_phases_ == 0)
    return false;
  float sum = 0.0f;
  for (uint8_t i = 0; i < this->meter_phases_; i++) {
    const sensor::Sensor *current = this->meter_currents_[i];
    const sensor::Sensor *voltage = this->meter_voltages_[i];
    if (current == nullptr || voltage == nullptr || !current->has_state() || !voltage->has_state() ||
        std::isnan(current->state) || std::isnan(voltage->state))
      return false;
    sum += current->state * voltage->state;
  }
  *power = sum;
  return true;
#else
  return false;
#endif
}

uint32_t Evse::get_session_time_() {
  if (!this->has_session_)
    return 0;
  return (this->now_ms() - this->session_start_) / 1000;
}

// --- LEDs ---

void Evse::update_leds_() {
#ifdef USE_LIGHT
  EvseState state = this->core_.get_state();
  if (this->led_state_valid_ && state == this->led_state_)
    return;
  this->led_state_valid_ = true;
  this->led_state_ = state;

  // same patterns as esp32-evse (main.c update_leds)
  switch (state) {
    case EvseState::A:
      this->charging_led_.set_pattern(0, 0);
      this->error_led_.set_pattern(0, 0);
      break;
    case EvseState::B1:
    case EvseState::B2:
      this->charging_led_.set_pattern(500, 500);
      this->error_led_.set_pattern(0, 0);
      break;
    case EvseState::C1:
    case EvseState::D1:
      this->charging_led_.set_pattern(1900, 100);
      this->error_led_.set_pattern(0, 0);
      break;
    case EvseState::C2:
    case EvseState::D2:
      this->charging_led_.set_pattern(1, 0);
      this->error_led_.set_pattern(0, 0);
      break;
    case EvseState::E:
      this->charging_led_.set_pattern(0, 0);
      this->error_led_.set_pattern(1, 0);
      break;
    case EvseState::F:
      this->charging_led_.set_pattern(0, 0);
      this->error_led_.set_pattern(500, 500);
      break;
  }
#endif
}

void Evse::update_wifi_led_() {
#ifdef USE_LIGHT
  if (!this->wifi_led_.has_light())
    return;
#ifdef USE_WIFI
  wifi::WiFiComponent *wifi = wifi::global_wifi_component;
  if (wifi == nullptr || wifi->is_disabled()) {
    this->wifi_led_.set_pattern(0, 0);
  } else if (wifi->is_connected()) {
    this->wifi_led_.set_pattern(1, 0);
  } else if (wifi->is_ap_active()) {
    this->wifi_led_.set_pattern(100, 900);  // access point (fallback hotspot) active
  } else {
    this->wifi_led_.set_pattern(500, 500);  // connecting
  }
#else
  this->wifi_led_.set_pattern(0, 0);
#endif
#endif
}

// ---------------------------------------------------------------------------
// EvseHal

uint64_t Evse::now_ms() {
  uint32_t now = millis();
  if (now < this->last_millis_)
    this->millis_high_ += 1ULL << 32;
  this->last_millis_ = now;
  return this->millis_high_ + now;
}

void Evse::pilot_measure(PilotVoltage *up_voltage, bool *down_voltage_n12) {
  float high_samples[PILOT_HI_LO_SAMPLES];
  float low_samples[PILOT_HI_LO_SAMPLES];

  for (int i = 0; i < PILOT_HI_LO_SAMPLES; i++) {
    high_samples[i] = 0;
    low_samples[i] = 3300;
  }

  for (int i = 0; i < PILOT_SAMPLES; i++) {
    float sample = this->pilot_sensor_->sample();
    if (!std::isnan(sample)) {
      float mv = sample * 1000.0f;

      for (int j = 0; j < PILOT_HI_LO_SAMPLES; j++) {
        if (mv > high_samples[j]) {
          for (int m = PILOT_HI_LO_SAMPLES - 1; m > j; m--)
            high_samples[m] = high_samples[m - 1];
          high_samples[j] = mv;
          break;
        }
      }

      for (int j = 0; j < PILOT_HI_LO_SAMPLES; j++) {
        if (mv < low_samples[j]) {
          for (int m = PILOT_HI_LO_SAMPLES - 1; m > j; m--)
            low_samples[m] = low_samples[m - 1];
          low_samples[j] = mv;
          break;
        }
      }
    }

    delayMicroseconds(1000 / PILOT_SAMPLES);  // 1000us pilot period
  }

  float high = 0;
  float low = 0;
  for (int i = 0; i < PILOT_HI_LO_SAMPLES; i++) {
    high += high_samples[i];
    low += low_samples[i];
  }
  high /= PILOT_HI_LO_SAMPLES;
  low /= PILOT_HI_LO_SAMPLES;

  this->pilot_high_mv_ = high;
  this->pilot_low_mv_ = low;

  if (high >= this->pilot_levels_[0]) {
    *up_voltage = PilotVoltage::V12;
  } else if (high >= this->pilot_levels_[1]) {
    *up_voltage = PilotVoltage::V9;
  } else if (high >= this->pilot_levels_[2]) {
    *up_voltage = PilotVoltage::V6;
  } else if (high >= this->pilot_levels_[3]) {
    *up_voltage = PilotVoltage::V3;
  } else {
    *up_voltage = PilotVoltage::V1;
  }

  *down_voltage_n12 = low <= this->pilot_levels_[4];

  ESP_LOGVV(TAG, "Pilot measure: %.0fmV - %.0fmV, up %u, down -12V %d", low, high, (unsigned) *up_voltage,
            (int) *down_voltage_n12);
}

void Evse::pilot_set_level(bool level) {
  ESP_LOGD(TAG, "Set pilot level %s", level ? "+12V" : "-12V");
  this->pilot_output_->set_level(level ? 1.0f : 0.0f);
}

void Evse::pilot_set_amps(uint16_t amps_x10) {
  float amps = amps_x10 / 10.0f;
  float duty;  // %
  if (amps_x10 >= 60 && amps_x10 <= 510) {
    // amps = (duty cycle %) * 0.6
    duty = amps / 0.6f;
  } else if (amps_x10 > 510 && amps_x10 <= 800) {
    // amps = (duty cycle % - 64) * 2.5
    duty = amps / 2.5f + 64.0f;
  } else {
    ESP_LOGE(TAG, "Try set invalid ampere value %uA*10", (unsigned) amps_x10);
    return;
  }

  ESP_LOGD(TAG, "Set pilot PWM %.1fA, duty %.2f%%", amps, duty);
  this->pilot_output_->set_level(duty / 100.0f);
}

void Evse::write_relay_(bool state) {
  if (state) {
    this->relay_->turn_on();
  } else {
    this->relay_->turn_off();
  }
}

void Evse::ac_relay_set_state(bool state) {
  if (state != this->relay_state_)
    ESP_LOGI(TAG, "Set AC relay %s", ONOFF(state));
  this->relay_state_ = state;
  this->write_relay_(state);
}

bool Evse::rcm_is_triggered() { return this->rcm_.is_triggered(); }

void Evse::rcm_selftest_start() { this->rcm_.selftest_start(); }

RcmSelftestResult Evse::rcm_selftest_poll() { return this->rcm_.selftest_poll(); }

void Evse::rcm_selftest_cancel() { this->rcm_.selftest_cancel(); }

bool Evse::temperature_get_high(float *high) {
  bool valid = false;
  float value = 0;
  for (const auto &temp : this->temperature_sensors_) {
    if (temp.has_value && (!valid || temp.value > value)) {
      value = temp.value;
      valid = true;
    }
  }
  *high = value;
  return valid;
}

bool Evse::temperature_is_error() {
  uint32_t now = millis();
  for (const auto &temp : this->temperature_sensors_) {
    uint32_t since = temp.has_value ? temp.last_valid : this->setup_time_;
    if (now - since > this->temperature_timeout_)
      return true;
  }
  return false;
}

bool Evse::auth_is_active() {
#ifdef USE_BINARY_SENSOR
  if (this->auth_binary_sensor_ != nullptr && this->auth_binary_sensor_->has_state() &&
      this->auth_binary_sensor_->state)
    return true;
#endif
  if (this->auth_switch_ != nullptr && this->auth_switch_->state)
    return true;
  return false;
}

bool Evse::energy_meter_is_available() { return this->meter_phases_ > 0; }

void Evse::energy_meter_start_session() {
  if (this->has_session_)
    return;
  ESP_LOGI(TAG, "Start session");
  this->has_session_ = true;
  this->session_start_ = this->now_ms();
  this->charging_time_ = 0;
  this->session_energy_ = 0.0;
}

void Evse::energy_meter_stop_session() {
  if (!this->has_session_)
    return;
  ESP_LOGI(TAG, "Stop session, energy %.0fWh, session time %" PRIu32 "s, charging time %" PRIu32 "s",
           this->session_energy_, this->get_session_time_(), (uint32_t) (this->charging_time_ / 1000));
  this->has_session_ = false;
  this->session_start_ = 0;
  this->charging_time_ = 0;
  this->session_energy_ = 0.0;
}

void Evse::energy_meter_process(bool charging, uint16_t /*charging_current_x10*/) {
  uint64_t now = this->now_ms();
  uint64_t delta = this->last_process_time_ != 0 ? now - this->last_process_time_ : 0;
  this->last_process_time_ = now;

  if (charging)
    this->charging_time_ += delta;

  // Session energy from the meter values, only while the relay is closed: with an open relay
  // no current flows, whatever the (noisy) meter shows.
  float power;
  if (this->has_session_ && this->relay_state_ && this->meter_power_(&power) && power > 0.0f)
    this->session_energy_ += static_cast<double>(power) * delta / 3600000.0;
}

uint32_t Evse::energy_meter_get_consumption() { return static_cast<uint32_t>(this->session_energy_); }

uint32_t Evse::energy_meter_get_charging_time() { return static_cast<uint32_t>(this->charging_time_ / 1000); }

uint32_t Evse::energy_meter_get_power() {
  if (!this->relay_state_)
    return 0;  // no current flows with an open relay
  float power;
  if (!this->meter_power_(&power))
    return UINT32_MAX;  // unknown, the under power limit is not reached
  return static_cast<uint32_t>(std::max(lroundf(power), 0L));
}

#ifdef USE_SENSOR
void Evse::add_temperature_sensor(sensor::Sensor *sensor) {
  this->temperature_sensors_.push_back(TemperatureSensor{sensor, NAN, 0, false});
}
#endif

}  // namespace evse
}  // namespace esphome
