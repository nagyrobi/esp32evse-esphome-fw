#pragma once

// Hardware independent EVSE state machine.
//
// This is a C++ port of components/evse/src/evse.c from esp32-evse
// (https://github.com/dzurikmiroslav/esp32-evse). It intentionally has no
// dependency on ESPHome so that it can be unit tested on the host
// (see tests/host). All hardware access goes through the EvseHal interface,
// which is implemented by the ESPHome component (evse.h) and by the test mocks.
//
// Differences to the original (all deliberate):
//  * The RCM self test is asynchronous (the original blocks up to 500 ms).
//    While it is pending, charging is not allowed.
//  * Lowering the maximum charging current also clamps the charging current.
//  * Authorization may additionally be granted by a level ("source active").
//  * Without an energy meter the consumption / under power limits are ignored.
//  * When all errors clear, the state machine restarts from state A. The original
//    resumed the state from before the error (e.g. C2 -> relay on) when a non
//    auto-clear error (temperature) cleared.
//  * Socket outlet (proximity, socket lock) support is not implemented.

#include <cstdint>

namespace esphome {
namespace evse {

/// States of the EVSE controller (SAE J1772 / IEC 61851-1)
enum class EvseState : uint8_t {
  A = 0,  ///< Standby, no vehicle
  B1,     ///< Vehicle connected, charging not allowed (pilot +12V/+9V DC)
  B2,     ///< Vehicle connected, ready (pilot PWM)
  C1,     ///< Vehicle requests charging, charging not allowed
  C2,     ///< Charging
  D1,     ///< Vehicle requests charging with ventilation, charging not allowed
  D2,     ///< Charging with ventilation
  E,      ///< Error
  F,      ///< EVSE not available
};

/// Positive ("up") pilot voltage level as seen by the EVSE
enum class PilotVoltage : uint8_t {
  V12 = 0,
  V9,
  V6,
  V3,
  V1,  ///< below 3V, invalid
};

enum class RcmSelftestResult : uint8_t {
  PENDING = 0,
  PASSED,
  FAILED,
};

// Error bits, same values as EVSE_ERR_* in esp32-evse.
// Bits 2 and 3 (socket lock / unlock fault) are reserved, socket lock is not supported.
static constexpr uint32_t ERR_PILOT_FAULT_BIT = 1UL << 0;
static constexpr uint32_t ERR_DIODE_SHORT_BIT = 1UL << 1;
static constexpr uint32_t ERR_RCM_TRIGGERED_BIT = 1UL << 4;
static constexpr uint32_t ERR_RCM_SELFTEST_FAULT_BIT = 1UL << 5;
static constexpr uint32_t ERR_TEMPERATURE_HIGH_BIT = 1UL << 6;
static constexpr uint32_t ERR_TEMPERATURE_FAULT_BIT = 1UL << 7;

/// Errors cleared automatically after ERROR_WAIT_TIME
static constexpr uint32_t ERR_AUTO_CLEAR_BITS =
    ERR_PILOT_FAULT_BIT | ERR_DIODE_SHORT_BIT | ERR_RCM_TRIGGERED_BIT | ERR_RCM_SELFTEST_FAULT_BIT;

static constexpr uint8_t LIMIT_CONSUMPTION_BIT = 1 << 0;
static constexpr uint8_t LIMIT_CHARGING_TIME_BIT = 1 << 1;
static constexpr uint8_t LIMIT_UNDER_POWER_BIT = 1 << 2;

static constexpr uint8_t MAX_CHARGING_CURRENT_MIN = 6;      // A
static constexpr uint8_t MAX_CHARGING_CURRENT_MAX = 63;     // A
static constexpr uint16_t CHARGING_CURRENT_MIN = 60;        // A*10
static constexpr uint32_t AUTHORIZED_TIME = 60000;          // ms, default validity of an authorization
static constexpr uint32_t ERROR_WAIT_TIME = 60000;          // ms, auto clear of errors
static constexpr uint32_t UNDER_POWER_TIME = 60000;         // ms
static constexpr uint32_t C1_D1_AC_RELAY_WAIT_TIME = 6000;  // ms
static constexpr uint8_t TEMP_THRESHOLD_MIN = 40;           // °C
static constexpr uint8_t TEMP_THRESHOLD_MAX = 80;           // °C

inline bool evse_state_is_session(EvseState state) { return state >= EvseState::B1 && state <= EvseState::D2; }
inline bool evse_state_is_charging(EvseState state) { return state == EvseState::C2 || state == EvseState::D2; }
const char *evse_state_to_str(EvseState state);

/// Hardware abstraction used by EvseCore.
class EvseHal {
 public:
  virtual ~EvseHal() = default;

  /// Monotonic time in milliseconds, must be > 0
  virtual uint64_t now_ms() = 0;

  /// Measure the pilot up voltage and whether the down voltage reaches -12V
  virtual void pilot_measure(PilotVoltage *up_voltage, bool *down_voltage_n12) = 0;
  /// Constant +12V (true) or -12V (false) pilot
  virtual void pilot_set_level(bool level) = 0;
  /// 1kHz PWM pilot advertising the given current
  virtual void pilot_set_amps(uint16_t amps_x10) = 0;

  virtual void ac_relay_set_state(bool state) = 0;

  /// Residual current monitor detected a leakage (edge since last call, confirmed by level)
  virtual bool rcm_is_triggered() { return false; }
  virtual void rcm_selftest_start() {}
  virtual RcmSelftestResult rcm_selftest_poll() { return RcmSelftestResult::PASSED; }
  virtual void rcm_selftest_cancel() {}

  /// Highest temperature of all sensors in °C, false if no valid value is known
  virtual bool temperature_get_high(float *high) { return false; }
  /// Temperature sensor(s) faulty
  virtual bool temperature_is_error() { return false; }

  /// The authorization source (binary sensor / switch) is active
  virtual bool auth_is_active() { return false; }

  /// Whether power / consumption values are available
  virtual bool energy_meter_is_available() { return true; }
  virtual void energy_meter_start_session() {}
  virtual void energy_meter_stop_session() {}
  /// Called at the end of each process() iteration
  virtual void energy_meter_process(bool charging, uint16_t charging_current_x10) {}
  /// Session consumption in Wh
  virtual uint32_t energy_meter_get_consumption() { return 0; }
  /// Session charging time in s
  virtual uint32_t energy_meter_get_charging_time() { return 0; }
  /// Actual power in W
  virtual uint32_t energy_meter_get_power() { return 0; }
};

class EvseCore {
 public:
  explicit EvseCore(EvseHal *hal) : hal_(hal) {}

  /// Set initial outputs (pilot +12V, relay off), like evse_init()
  void init();
  /// Reset run time state, used only in tests
  void reset();
  /// Main loop of the state machine, call every 50ms
  void process();

  /// Current state, E when there is any error
  EvseState get_state() const { return this->error_ ? EvseState::E : this->state_; }
  /// Error bits (ERR_*_BIT)
  uint32_t get_error() const { return this->error_; }

  bool is_available() const { return this->available_; }
  /// When not available the state machine goes to (and stays in) state F
  void set_available(bool available);

  bool is_enabled() const { return this->enabled_; }
  /// Allow the EVSE to actually charge the vehicle
  void set_enabled(bool enabled);

  /// Maximum charging current in A
  uint8_t get_max_charging_current() const { return this->max_charging_current_; }
  bool set_max_charging_current(uint8_t max_charging_current);

  /// Charging current in A*10
  uint16_t get_charging_current() const { return this->charging_current_; }
  bool set_charging_current(uint16_t charging_current);

  bool is_require_auth() const { return this->require_auth_; }
  void set_require_auth(bool require_auth) { this->require_auth_ = require_auth; }
  /// Validity of a grant made by authorize() in ms
  void set_auth_timeout(uint32_t auth_timeout) { this->auth_timeout_ = auth_timeout; }
  /// Authorize to start charging (valid for the auth timeout, consumed by a session)
  void authorize();
  bool is_pending_auth() const;
  bool is_authorized() const { return this->authorized_; }

  bool is_limit_reached() const { return this->reached_limit_ != 0; }
  uint8_t get_reached_limit() const { return this->reached_limit_; }

  /// Consumption limit in Wh, 0 = disabled
  uint32_t get_consumption_limit() const { return this->consumption_limit_; }
  void set_consumption_limit(uint32_t consumption_limit) { this->consumption_limit_ = consumption_limit; }
  /// Charging time limit in s, 0 = disabled
  uint32_t get_charging_time_limit() const { return this->charging_time_limit_; }
  void set_charging_time_limit(uint32_t charging_time_limit) { this->charging_time_limit_ = charging_time_limit; }
  /// Under power limit in W, 0 = disabled
  uint32_t get_under_power_limit() const { return this->under_power_limit_; }
  void set_under_power_limit(uint32_t under_power_limit) { this->under_power_limit_ = under_power_limit; }

  /// Residual current monitoring
  bool is_rcm() const { return this->rcm_; }
  void set_rcm(bool rcm) { this->rcm_ = rcm; }
  bool is_rcm_selftest_pending() const { return this->rcm_selftest_pending_; }

  /// Temperature monitoring
  bool is_temperature_monitoring() const { return this->temperature_monitoring_; }
  void set_temperature_monitoring(bool monitoring) { this->temperature_monitoring_ = monitoring; }
  /// Temperature threshold in °C
  uint8_t get_temp_threshold() const { return this->temp_threshold_; }
  bool set_temp_threshold(uint8_t temp_threshold);

 protected:
  enum class PilotState : uint8_t { V12, N12V, PWM };

  void set_timeout_(uint64_t *to, uint32_t ms);
  bool is_expired_(uint64_t *to);
  void set_error_bits_(uint32_t bits);
  void clear_error_bits_(uint32_t bits);
  void set_pilot_(PilotState pilot_state);
  uint16_t pilot_current_() const;
  void perform_rcm_selftest_();
  void poll_rcm_selftest_();
  void enter_new_state_(EvseState state);
  bool charging_allowed_() const;
  void check_authorization_();
  void check_charging_limits_();
  void check_other_error_conditions_();

  EvseHal *hal_;

  EvseState state_{EvseState::A};  // does not hold the error state
  EvseState prev_state_{EvseState::A};
  uint32_t error_{0};
  bool error_cleared_{false};
  uint16_t charging_current_{320};
  uint32_t consumption_limit_{0};
  uint32_t charging_time_limit_{0};
  uint32_t under_power_limit_{0};
  uint8_t reached_limit_{0};
  bool rcm_{false};
  bool rcm_selftest_done_{false};
  bool rcm_selftest_pending_{false};
  bool temperature_monitoring_{false};
  uint8_t temp_threshold_{60};
  bool enabled_{true};
  bool available_{true};
  bool require_auth_{false};
  bool authorized_{false};
  uint32_t auth_timeout_{AUTHORIZED_TIME};
  uint64_t auth_grant_to_{0};
  uint64_t error_wait_to_{0};
  bool under_power_timing_{false};
  uint64_t under_power_start_time_{0};
  uint64_t c1_d1_ac_relay_wait_to_{0};
  uint8_t max_charging_current_{32};
  PilotState pilot_state_{PilotState::V12};
};

}  // namespace evse
}  // namespace esphome
