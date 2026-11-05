#include "evse_core.h"

#ifdef EVSE_HOST_TEST
#include <cinttypes>
#include <cstdio>
#define EVSE_LOG_(level, tag, ...) \
  do { \
    std::printf("[%s][%s] ", level, tag); \
    std::printf(__VA_ARGS__); \
    std::printf("\n"); \
  } while (0)
#define ESP_LOGI(tag, ...) EVSE_LOG_("I", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) EVSE_LOG_("W", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) EVSE_LOG_("D", tag, __VA_ARGS__)
#else
#include <cinttypes>
#include "esphome/core/log.h"
#endif

namespace esphome {
namespace evse {

static const char *const TAG = "evse";

const char *evse_state_to_str(EvseState state) {
  switch (state) {
    case EvseState::A:
      return "A";
    case EvseState::B1:
      return "B1";
    case EvseState::B2:
      return "B2";
    case EvseState::C1:
      return "C1";
    case EvseState::C2:
      return "C2";
    case EvseState::D1:
      return "D1";
    case EvseState::D2:
      return "D2";
    case EvseState::E:
      return "E";
    case EvseState::F:
      return "F";
    default:
      return "?";
  }
}

// set a timeout value in ms
void EvseCore::set_timeout_(uint64_t *to, uint32_t ms) { *to = this->hal_->now_ms() + ms; }

// check if timeout is expired (sets timer value to 0, if true), returns false if timer value is 0
bool EvseCore::is_expired_(uint64_t *to) {
  bool expired = *to != 0 && this->hal_->now_ms() > *to;
  if (expired)
    *to = 0;
  return expired;
}

void EvseCore::set_error_bits_(uint32_t bits) {
  this->error_ |= bits;
  if (bits & ERR_AUTO_CLEAR_BITS) {
    this->set_timeout_(&this->error_wait_to_, ERROR_WAIT_TIME);
  }
}

void EvseCore::clear_error_bits_(uint32_t bits) {
  bool has_error = this->error_ != 0;
  this->error_ &= ~bits;
  if (has_error && this->error_ == 0) {
    this->error_cleared_ = true;
    // Restart from A. The state machine does not run while in error, so state_ still
    // holds the state before the error (e.g. C2). Resuming it would close the relay
    // without checking the pilot, authorization and session (esp32-evse does that
    // when a non auto-clear error, like the temperature, clears).
    this->state_ = EvseState::A;
  }
}

uint16_t EvseCore::pilot_current_() const {
  uint16_t max = this->max_charging_current_ * 10;
  return this->charging_current_ < max ? this->charging_current_ : max;
}

void EvseCore::set_pilot_(PilotState pilot_state) {
  if (this->pilot_state_ != pilot_state) {
    this->pilot_state_ = pilot_state;
    switch (pilot_state) {
      case PilotState::V12:
        this->hal_->pilot_set_level(true);
        break;
      case PilotState::N12V:
        this->hal_->pilot_set_level(false);
        break;
      case PilotState::PWM:
        this->hal_->pilot_set_amps(this->pilot_current_());
        break;
    }
  }
}

// start the RCM self test if RCM is enabled and the test was not yet done in this session
void EvseCore::perform_rcm_selftest_() {
  if (this->rcm_ && !this->rcm_selftest_done_ && !this->rcm_selftest_pending_) {
    ESP_LOGD(TAG, "Residual current monitor self test start");
    this->rcm_selftest_pending_ = true;
    this->hal_->rcm_selftest_start();
  }
}

void EvseCore::poll_rcm_selftest_() {
  if (!this->rcm_selftest_pending_)
    return;

  switch (this->hal_->rcm_selftest_poll()) {
    case RcmSelftestResult::PENDING:
      break;
    case RcmSelftestResult::PASSED:
      ESP_LOGI(TAG, "Residual current monitor self test success");
      this->rcm_selftest_pending_ = false;
      this->rcm_selftest_done_ = true;
      break;
    case RcmSelftestResult::FAILED:
      ESP_LOGW(TAG, "Residual current monitor self test fail");
      this->rcm_selftest_pending_ = false;
      this->rcm_selftest_done_ = true;
      this->set_error_bits_(ERR_RCM_SELFTEST_FAULT_BIT);
      break;
  }
}

void EvseCore::enter_new_state_(EvseState state) {
  switch (state) {
    case EvseState::A:
    case EvseState::E:
    case EvseState::F:
      this->set_pilot_(state == EvseState::A ? PilotState::V12 : PilotState::N12V);
      this->hal_->ac_relay_set_state(false);
      this->c1_d1_ac_relay_wait_to_ = 0;
      this->authorized_ = false;
      this->reached_limit_ = 0;
      this->under_power_timing_ = false;
      this->rcm_selftest_done_ = false;
      if (this->rcm_selftest_pending_) {
        this->rcm_selftest_pending_ = false;
        this->hal_->rcm_selftest_cancel();
      }
      this->hal_->energy_meter_stop_session();
      break;
    case EvseState::B1:
      this->set_pilot_(PilotState::V12);
      this->hal_->ac_relay_set_state(false);
      this->c1_d1_ac_relay_wait_to_ = 0;
      this->perform_rcm_selftest_();
      this->hal_->energy_meter_start_session();
      break;
    case EvseState::B2:
      this->set_pilot_(PilotState::PWM);
      this->hal_->ac_relay_set_state(false);
      break;
    case EvseState::C1:
    case EvseState::D1:
      this->set_pilot_(PilotState::V12);
      this->set_timeout_(&this->c1_d1_ac_relay_wait_to_, C1_D1_AC_RELAY_WAIT_TIME);
      break;
    case EvseState::C2:
    case EvseState::D2:
      this->set_pilot_(PilotState::PWM);
      this->hal_->ac_relay_set_state(true);
      break;
  }
}

bool EvseCore::charging_allowed_() const {
  if (!this->enabled_ || !this->available_ || !this->authorized_ || this->reached_limit_) {
    return false;
  }
  // charging is not allowed until the RCM self test passed
  if (this->rcm_selftest_pending_) {
    return false;
  }
  return true;
}

void EvseCore::check_authorization_() {
  if (!this->authorized_) {
    if (this->require_auth_) {
      uint64_t now = this->hal_->now_ms();
      this->authorized_ = (this->auth_grant_to_ != 0 && this->auth_grant_to_ >= now) || this->hal_->auth_is_active();
      this->auth_grant_to_ = 0;  // in any case we need a fresh authorization, if the EV is disconnected
      if (this->authorized_) {
        ESP_LOGI(TAG, "Authorized");
      }
    } else {
      this->authorized_ = true;
    }
  }
}

// set and clear limit bits in reached_limit
void EvseCore::check_charging_limits_() {
  bool meter = this->hal_->energy_meter_is_available();

  // check consumption limit
  if (meter && this->consumption_limit_ > 0 && this->hal_->energy_meter_get_consumption() > this->consumption_limit_) {
    this->reached_limit_ |= LIMIT_CONSUMPTION_BIT;
  } else {
    this->reached_limit_ &= ~LIMIT_CONSUMPTION_BIT;
  }

  // check charging time limit
  if (this->charging_time_limit_ > 0 && this->hal_->energy_meter_get_charging_time() > this->charging_time_limit_) {
    this->reached_limit_ |= LIMIT_CHARGING_TIME_BIT;
  } else {
    this->reached_limit_ &= ~LIMIT_CHARGING_TIME_BIT;
  }

  // check under power limit
  if (evse_state_is_charging(this->state_)) {
    uint64_t now = this->hal_->now_ms();
    if (meter && this->under_power_limit_ > 0 && this->hal_->energy_meter_get_power() < this->under_power_limit_) {
      if (!this->under_power_timing_) {
        this->under_power_timing_ = true;
        this->under_power_start_time_ = now;
      }
    } else {
      this->under_power_timing_ = false;
    }

    if (this->under_power_timing_ && this->under_power_start_time_ + UNDER_POWER_TIME < now) {
      this->reached_limit_ |= LIMIT_UNDER_POWER_BIT;
    } else {
      this->reached_limit_ &= ~LIMIT_UNDER_POWER_BIT;
    }
  }
}

// set and clear error condition bits, depending on the configuration
void EvseCore::check_other_error_conditions_() {
  if (this->rcm_ && !this->rcm_selftest_pending_ && this->hal_->rcm_is_triggered()) {
    this->set_error_bits_(ERR_RCM_TRIGGERED_BIT);
  }

  if (this->temperature_monitoring_) {
    float high;
    if (this->hal_->temperature_get_high(&high) && high > this->temp_threshold_) {
      this->set_error_bits_(ERR_TEMPERATURE_HIGH_BIT);
    } else {
      this->clear_error_bits_(ERR_TEMPERATURE_HIGH_BIT);
    }

    if (this->hal_->temperature_is_error()) {
      this->set_error_bits_(ERR_TEMPERATURE_FAULT_BIT);
    } else {
      this->clear_error_bits_(ERR_TEMPERATURE_FAULT_BIT);
    }
  }
}

void EvseCore::process() {
  PilotVoltage pilot_voltage;
  bool pilot_down_voltage_n12;
  this->hal_->pilot_measure(&pilot_voltage, &pilot_down_voltage_n12);

  if (this->is_expired_(&this->error_wait_to_)) {
    this->clear_error_bits_(ERR_AUTO_CLEAR_BITS);
    this->state_ = EvseState::A;
  }

  if (this->pilot_state_ == PilotState::PWM && !pilot_down_voltage_n12) {
    this->set_error_bits_(ERR_DIODE_SHORT_BIT);
  }

  // check configuration specific errors
  this->check_other_error_conditions_();

  // result of the RCM self test started on entering B1
  this->poll_rcm_selftest_();

  if (this->error_ == 0 && !this->error_cleared_) {
    // no errors
    // after clear error, process on next iteration, after apply_state

    uint8_t prev_reached_limit = this->reached_limit_;

    // set limit bits in reached_limit
    this->check_charging_limits_();

    switch (this->state_) {
      case EvseState::A:
        if (!this->available_) {
          this->state_ = EvseState::F;
          break;
        }
        switch (pilot_voltage) {
          case PilotVoltage::V12:
            // stay in current state
            break;
          case PilotVoltage::V9:
            this->state_ = EvseState::B1;
            break;
          default:
            this->set_error_bits_(ERR_PILOT_FAULT_BIT);
        }
        break;
      case EvseState::B1:
        this->check_authorization_();
        [[fallthrough]];
      case EvseState::B2:
        if (!this->available_) {
          this->state_ = EvseState::F;
          break;
        }
        switch (pilot_voltage) {
          case PilotVoltage::V12:
            this->state_ = EvseState::A;
            break;
          case PilotVoltage::V9:
            this->state_ = this->charging_allowed_() ? EvseState::B2 : EvseState::B1;
            break;
          case PilotVoltage::V6:
            this->state_ = this->charging_allowed_() ? EvseState::C2 : EvseState::C1;
            break;
          default:
            this->set_error_bits_(ERR_PILOT_FAULT_BIT);
        }
        break;
      case EvseState::C1:
        this->check_authorization_();
        if (this->is_expired_(&this->c1_d1_ac_relay_wait_to_)) {
          ESP_LOGW(TAG, "Force switch off ac relay");
          this->hal_->ac_relay_set_state(false);
          if (!this->available_) {
            this->state_ = EvseState::F;
            break;
          }
        }
        [[fallthrough]];
      case EvseState::C2:
        switch (pilot_voltage) {
          case PilotVoltage::V12:
            this->state_ = EvseState::A;
            break;
          case PilotVoltage::V9:
            this->state_ = this->charging_allowed_() ? EvseState::B2 : EvseState::B1;
            break;
          case PilotVoltage::V6:
            this->state_ = this->charging_allowed_() ? EvseState::C2 : EvseState::C1;
            break;
          case PilotVoltage::V3:
            this->state_ = this->charging_allowed_() ? EvseState::D2 : EvseState::D1;
            break;
          default:
            this->set_error_bits_(ERR_PILOT_FAULT_BIT);
        }
        break;
      case EvseState::D1:
        this->check_authorization_();
        if (this->is_expired_(&this->c1_d1_ac_relay_wait_to_)) {
          ESP_LOGW(TAG, "Force switch off ac relay");
          this->hal_->ac_relay_set_state(false);
          if (!this->available_) {
            this->state_ = EvseState::F;
            break;
          }
        }
        [[fallthrough]];
      case EvseState::D2:
        switch (pilot_voltage) {
          case PilotVoltage::V6:
            this->state_ = this->charging_allowed_() ? EvseState::C2 : EvseState::C1;
            break;
          case PilotVoltage::V3:
            this->state_ = this->charging_allowed_() ? EvseState::D2 : EvseState::D1;
            break;
          default:
            this->set_error_bits_(ERR_PILOT_FAULT_BIT);
        }
        break;
      case EvseState::E:
        break;
      case EvseState::F:
        if (this->available_) {
          this->state_ = EvseState::A;
        }
        break;
    }

    if (this->reached_limit_ != 0 && prev_reached_limit == 0) {
      ESP_LOGI(TAG, "Reached limit %u", (unsigned) this->reached_limit_);
    }
  }

  EvseState new_state = this->get_state();  // getter method detect error state

  if (this->prev_state_ != new_state) {
    ESP_LOGI(TAG, "Enter %s state", evse_state_to_str(new_state));
    if (this->error_) {
      ESP_LOGI(TAG, "Error bits %" PRIu32, this->error_);
    }
    this->enter_new_state_(new_state);
    this->prev_state_ = new_state;
  }

  this->error_cleared_ = false;

  this->hal_->energy_meter_process(evse_state_is_charging(this->get_state()), this->pilot_current_());
}

void EvseCore::init() {
  this->pilot_state_ = PilotState::V12;
  this->hal_->pilot_set_level(true);
  this->hal_->ac_relay_set_state(false);
}

void EvseCore::reset() {
  this->state_ = EvseState::A;
  this->prev_state_ = EvseState::A;
  this->error_ = 0;
  this->error_cleared_ = false;
  this->consumption_limit_ = 0;
  this->charging_time_limit_ = 0;
  this->under_power_limit_ = 0;
  this->reached_limit_ = 0;
  this->rcm_selftest_done_ = false;
  this->rcm_selftest_pending_ = false;
  this->enabled_ = true;
  this->available_ = true;
  this->require_auth_ = false;
  this->authorized_ = false;
  this->auth_grant_to_ = 0;
  this->error_wait_to_ = 0;
  this->under_power_timing_ = false;
  this->under_power_start_time_ = 0;
  this->c1_d1_ac_relay_wait_to_ = 0;
  this->pilot_state_ = PilotState::V12;
}

bool EvseCore::set_max_charging_current(uint8_t max_charging_current) {
  if (max_charging_current < MAX_CHARGING_CURRENT_MIN || max_charging_current > MAX_CHARGING_CURRENT_MAX) {
    ESP_LOGW(TAG, "Max charging current %uA out of range", (unsigned) max_charging_current);
    return false;
  }

  ESP_LOGI(TAG, "Set max charging current %uA", (unsigned) max_charging_current);
  this->max_charging_current_ = max_charging_current;

  // never advertise more than the maximum
  if (this->charging_current_ > max_charging_current * 10) {
    this->set_charging_current(max_charging_current * 10);
  }

  return true;
}

bool EvseCore::set_charging_current(uint16_t charging_current) {
  if (charging_current < CHARGING_CURRENT_MIN || charging_current > this->max_charging_current_ * 10) {
    ESP_LOGW(TAG, "Charging current %uA*10 out of range", (unsigned) charging_current);
    return false;
  }

  ESP_LOGI(TAG, "Set charging current %uA*10", (unsigned) charging_current);
  this->charging_current_ = charging_current;

  if (this->pilot_state_ == PilotState::PWM) {
    this->hal_->pilot_set_amps(this->pilot_current_());
  }

  return true;
}

bool EvseCore::set_temp_threshold(uint8_t temp_threshold) {
  if (temp_threshold < TEMP_THRESHOLD_MIN || temp_threshold > TEMP_THRESHOLD_MAX) {
    ESP_LOGW(TAG, "Temperature threshold %u°C out of range", (unsigned) temp_threshold);
    return false;
  }
  this->temp_threshold_ = temp_threshold;
  return true;
}

void EvseCore::authorize() {
  ESP_LOGI(TAG, "Authorize");
  this->set_timeout_(&this->auth_grant_to_, this->auth_timeout_);
  this->under_power_timing_ = false;
}

bool EvseCore::clear_error() {
  if ((this->error_ & ERR_AUTO_CLEAR_BITS) == 0) {
    ESP_LOGI(TAG, "Clear error: nothing to clear%s",
             this->error_ ? ", temperature errors clear when the temperature is back to normal" : "");
    return false;
  }
  ESP_LOGI(TAG, "Clear error");
  // same as the expiry of the error wait time in process()
  this->error_wait_to_ = 0;
  this->clear_error_bits_(ERR_AUTO_CLEAR_BITS);
  this->state_ = EvseState::A;
  return true;
}

bool EvseCore::is_pending_auth() const {
  return this->error_ == 0 && evse_state_is_session(this->state_) && !this->authorized_;
}

// allow the EVSE to actually charge the vehicle, it will wait until enabled
void EvseCore::set_enabled(bool enabled) {
  if (this->enabled_ != enabled) {
    ESP_LOGI(TAG, "Set enabled %d", (int) enabled);
    this->enabled_ = enabled;
  }
}

// allow the EVSE state machine to act, if available is false the state machine will stay in state F
void EvseCore::set_available(bool available) {
  if (this->available_ != available) {
    ESP_LOGI(TAG, "Set available %d", (int) available);
    this->available_ = available;
  }
}

}  // namespace evse
}  // namespace esphome
