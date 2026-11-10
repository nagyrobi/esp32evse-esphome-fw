// Host unit tests of the EVSE state machine (components/evse/evse_core.*).
//
// The first group of tests is a 1:1 port of test_app/main/test_evse.c from
// esp32-evse, the second group covers behaviour specific to this port.
//
// Build and run: tests/host/run.sh

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "evse_core.h"

using namespace esphome::evse;

// ---------------------------------------------------------------------------
// Minimal test framework

static int g_failures = 0;
static const char *g_current_test = "";

#define TEST_FAIL_(msg, file, line) \
  do { \
    std::printf("  FAIL %s:%d (%s): %s\n", file, line, g_current_test, msg); \
    g_failures++; \
    throw 1; \
  } while (0)

#define TEST_ASSERT_TRUE(cond) \
  do { \
    if (!(cond)) \
      TEST_FAIL_("expected true: " #cond, __FILE__, __LINE__); \
  } while (0)
#define TEST_ASSERT_FALSE(cond) \
  do { \
    if (cond) \
      TEST_FAIL_("expected false: " #cond, __FILE__, __LINE__); \
  } while (0)
#define TEST_ASSERT_EQUAL(expected, actual) \
  do { \
    if (!((expected) == (actual))) { \
      char buf_[256]; \
      std::snprintf(buf_, sizeof(buf_), "expected %s == %s (%ld != %ld)", #expected, #actual, (long) (expected), \
                    (long) (actual)); \
      TEST_FAIL_(buf_, __FILE__, __LINE__); \
    } \
  } while (0)

struct TestCase {
  const char *name;
  std::function<void()> fn;
};
static std::vector<TestCase> &tests() {
  static std::vector<TestCase> t;
  return t;
}
struct TestRegistrar {
  TestRegistrar(const char *name, std::function<void()> fn) { tests().push_back({name, fn}); }
};
#define TEST(name) \
  static void test_##name(); \
  static TestRegistrar registrar_##name(#name, test_##name); \
  static void test_##name()

// ---------------------------------------------------------------------------
// Mocks (equivalent of test_app/mocks/peripherals)

enum PilotMockState { PILOT_MOCK_STATE_HIGH, PILOT_MOCK_STATE_LOW, PILOT_MOCK_STATE_PWM };

class MockHal : public EvseHal {
 public:
  // time
  uint64_t now = 1000;
  // EV side of the pilot
  PilotVoltage pilot_mock_up_voltage = PilotVoltage::V12;
  bool pilot_mock_down_voltage_n12 = false;
  // EVSE outputs
  PilotMockState pilot_mock_state = PILOT_MOCK_STATE_HIGH;
  uint16_t pilot_mock_amps = 0;
  bool ac_relay_mock_state = false;
  // energy meter
  bool energy_meter_mock_available = true;
  uint32_t energy_meter_mock_power = 0;
  uint32_t energy_meter_mock_charging_time = 0;
  uint32_t energy_meter_mock_consumption = 0;
  int session_starts = 0;
  int session_stops = 0;
  bool last_process_charging = false;
  uint16_t last_process_current = 0;
  // rcm
  bool rcm_mock_triggered = false;
  RcmSelftestResult rcm_mock_result = RcmSelftestResult::PASSED;
  int rcm_selftest_starts = 0;
  int rcm_selftest_polls = 0;
  int rcm_selftest_cancels = 0;
  int rcm_triggered_calls = 0;
  // temperature
  bool temp_mock_valid = false;
  float temp_mock_high = 25.0f;
  bool temp_mock_error = false;
  // authorization source
  bool auth_mock_active = false;

  void step(uint32_t ms) { this->now += ms; }  // vTaskStepTick

  uint64_t now_ms() override { return this->now; }
  void pilot_measure(PilotVoltage *up_voltage, bool *down_voltage_n12) override {
    *up_voltage = this->pilot_mock_up_voltage;
    *down_voltage_n12 = this->pilot_mock_down_voltage_n12;
  }
  void pilot_set_level(bool level) override {
    this->pilot_mock_state = level ? PILOT_MOCK_STATE_HIGH : PILOT_MOCK_STATE_LOW;
  }
  void pilot_set_amps(uint16_t amps_x10) override {
    this->pilot_mock_state = PILOT_MOCK_STATE_PWM;
    this->pilot_mock_amps = amps_x10;
  }
  void ac_relay_set_state(bool state) override { this->ac_relay_mock_state = state; }
  bool rcm_is_triggered() override {
    this->rcm_triggered_calls++;
    bool t = this->rcm_mock_triggered;
    this->rcm_mock_triggered = false;  // edge
    return t;
  }
  void rcm_selftest_start() override { this->rcm_selftest_starts++; }
  RcmSelftestResult rcm_selftest_poll() override {
    this->rcm_selftest_polls++;
    return this->rcm_mock_result;
  }
  void rcm_selftest_cancel() override { this->rcm_selftest_cancels++; }
  bool temperature_get_high(float *high) override {
    *high = this->temp_mock_high;
    return this->temp_mock_valid;
  }
  bool temperature_is_error() override { return this->temp_mock_error; }
  bool auth_is_active() override { return this->auth_mock_active; }
  bool energy_meter_is_available() override { return this->energy_meter_mock_available; }
  void energy_meter_start_session() override { this->session_starts++; }
  void energy_meter_stop_session() override { this->session_stops++; }
  void energy_meter_process(bool charging, uint16_t charging_current_x10) override {
    this->last_process_charging = charging;
    this->last_process_current = charging_current_x10;
  }
  uint32_t energy_meter_get_consumption() override { return this->energy_meter_mock_consumption; }
  uint32_t energy_meter_get_charging_time() override { return this->energy_meter_mock_charging_time; }
  uint32_t energy_meter_get_power() override { return this->energy_meter_mock_power; }
};

static MockHal hal;
static EvseCore evse(&hal);

// TEST_SETUP(evse)
static void setup() {
  hal = MockHal();
  evse = EvseCore(&hal);

  // EVSE:
  evse.reset();
  hal.ac_relay_mock_state = false;
  hal.pilot_mock_state = PILOT_MOCK_STATE_HIGH;

  // EV:
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  hal.pilot_mock_down_voltage_n12 = false;
  hal.energy_meter_mock_power = 0;
  hal.energy_meter_mock_charging_time = 0;
  hal.energy_meter_mock_consumption = 0;

  evse.process();

  // to make sure
  TEST_ASSERT_EQUAL(EvseState::A, evse.get_state());
  TEST_ASSERT_EQUAL(PILOT_MOCK_STATE_HIGH, hal.pilot_mock_state);
  TEST_ASSERT_FALSE(hal.ac_relay_mock_state);
}

#define ASSERT_EVSE(state, pilot, relay) \
  do { \
    TEST_ASSERT_EQUAL(state, evse.get_state()); \
    TEST_ASSERT_EQUAL(pilot, hal.pilot_mock_state); \
    TEST_ASSERT_EQUAL(relay, hal.ac_relay_mock_state); \
  } while (0)

// EV connect, EVSE stay in B2 state
static void ev_connect_sequence() {
  // EV connect: A -> B1
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // When all conditions are met (avalable, enabled, autorized...): B1 -> B2
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
}

// EV requires charging: B2 -> C2
static void ev_charge_sequence() {
  hal.pilot_mock_up_voltage = PilotVoltage::V6;
  hal.pilot_mock_down_voltage_n12 = true;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
}

// ===========================================================================
// Port of esp32-evse test_app/main/test_evse.c

TEST(ev_end_charging) {
  ev_connect_sequence();
  ev_charge_sequence();

  // EV request pause charging: C2 -> B2
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  // EV disconnect: B2 -> A
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
}

TEST(during_charging_disable_ev_react) {
  ev_connect_sequence();
  ev_charge_sequence();

  // EVSE disable, EV has 6sec to end charging: C2 -> C1
  evse.set_enabled(false);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // EV end charging: C1 -> B1
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  hal.pilot_mock_down_voltage_n12 = false;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // EVSE enable: B1 -> B2
  evse.set_enabled(true);
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  // EV require charging: B2 -> C2
  ev_charge_sequence();
}

TEST(during_charging_disable_ev_not_react) {
  ev_connect_sequence();
  ev_charge_sequence();

  // EVSE disable, EV has 6sec to end charging: C2 -> C1
  evse.set_enabled(false);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // EV not end charging, EVSE still connected AC
  hal.step(1000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // EV not end charging, EVSE force disconnect AC
  hal.step(5500);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, false);
}

TEST(before_charging_disable) {
  // EVSE set disabled
  evse.set_enabled(false);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);

  // EV connect: A -> B1
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // Still disabled
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // EVSE enable: B1 -> B2
  evse.set_enabled(true);
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  ev_charge_sequence();
}

TEST(during_charging_not_available_ev_react) {
  ev_connect_sequence();
  ev_charge_sequence();

  // EVSE set not available, EV has 6sec to end charging: C2 -> C1
  evse.set_available(false);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // EV end charging: C1 -> B1
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  hal.pilot_mock_down_voltage_n12 = false;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // EVSE in next iteration go to F state: B1 -> F
  evse.process();
  ASSERT_EVSE(EvseState::F, PILOT_MOCK_STATE_LOW, false);

  // EVSE set available: F -> A
  evse.set_available(true);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);

  // EV still connected, next iteration it detect: A -> B1
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // B1 -> B2
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  ev_charge_sequence();
}

TEST(during_charging_not_available_ev_not_react) {
  ev_connect_sequence();
  ev_charge_sequence();

  // EVSE go to no available, EV has 6sec to end charging: C2 -> C1
  evse.set_available(false);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // EV not end charging, EVSE still connected AC
  hal.step(1000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // EV not end charging, EVSE force disconnect AC: C1 -> F
  hal.step(5500);
  evse.process();
  ASSERT_EVSE(EvseState::F, PILOT_MOCK_STATE_LOW, false);
}

TEST(before_charging_not_available) {
  // EVSE set not available: A -> F
  evse.set_available(false);
  evse.process();
  ASSERT_EVSE(EvseState::F, PILOT_MOCK_STATE_LOW, false);

  // EV connect: F
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::F, PILOT_MOCK_STATE_LOW, false);

  // EVSE set available, first iteration go to A: F -> A
  evse.set_available(true);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);

  // A -> B1
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // B1 -> B2
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  ev_charge_sequence();
}

TEST(require_auth) {
  evse.set_require_auth(true);
  TEST_ASSERT_FALSE(evse.is_pending_auth());

  // EV connect: A -> B1
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_TRUE(evse.is_pending_auth());

  // Wait for authorization
  evse.process();
  TEST_ASSERT_EQUAL(EvseState::B1, evse.get_state());
  TEST_ASSERT_TRUE(evse.is_pending_auth());

  // B1 -> B2
  evse.authorize();
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  ev_charge_sequence();
}

TEST(pilot_error) {
  // Some invalid operation: A -> E
  hal.pilot_mock_up_voltage = PilotVoltage::V1;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_PILOT_FAULT_BIT & evse.get_error());

  // Stay in error
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_PILOT_FAULT_BIT & evse.get_error());

  // After 1min error cleared: E -> A
  hal.step(61000);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_EQUAL(0, evse.get_error());
}

TEST(charging_time_limit) {
  uint32_t limit = 3600;  // 1h (in s)
  evse.set_charging_time_limit(limit);

  ev_connect_sequence();
  ev_charge_sequence();

  // Limit not reached yet
  hal.energy_meter_mock_charging_time = limit - 10;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // Limit reached, stop charging: C2 -> C1
  hal.energy_meter_mock_charging_time = limit + 10;
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
  TEST_ASSERT_TRUE(evse.is_limit_reached());

  // Increase limit, start charging: C1 -> C2
  limit *= 2;
  evse.set_charging_time_limit(limit);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // Limit reached, stop charging: C2 -> C1
  hal.energy_meter_mock_charging_time = limit + 10;
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
}

TEST(consumption_limit) {
  uint32_t limit = 3600000;
  evse.set_consumption_limit(limit);

  ev_connect_sequence();
  ev_charge_sequence();

  // Limit not reached yet
  hal.energy_meter_mock_consumption = limit - 10;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // Limit reached, stop charging: C2 -> C1
  hal.energy_meter_mock_consumption = limit + 10;
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // Increase limit, start charging: C1 -> C2
  limit *= 2;
  evse.set_consumption_limit(limit);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // Limit reached, stop charging: C2 -> C1
  hal.energy_meter_mock_consumption = limit + 10;
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
}

TEST(under_power_limit) {
  uint16_t limit = 10000;  // 10kW (in W)
  evse.set_under_power_limit(limit);

  ev_connect_sequence();
  ev_charge_sequence();

  // Limit not reached yet
  hal.step(61000);  // must be 1min under limit to react
  hal.energy_meter_mock_power = limit + 10;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // Limit reached, not react yet
  hal.energy_meter_mock_power = limit - 10;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // Limit reached, not react yet
  hal.step(59000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // After 1min react: C2 -> C1
  hal.step(2000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
}

// ===========================================================================
// Tests of behaviour specific to this port

TEST(session_start_stop_and_meter_process) {
  ev_connect_sequence();
  TEST_ASSERT_EQUAL(1, hal.session_starts);
  TEST_ASSERT_FALSE(hal.last_process_charging);

  ev_charge_sequence();
  TEST_ASSERT_TRUE(hal.last_process_charging);
  TEST_ASSERT_EQUAL(320, hal.last_process_current);

  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_FALSE(hal.last_process_charging);
  TEST_ASSERT_TRUE(hal.session_stops >= 1);
}

TEST(diode_short) {
  ev_connect_sequence();

  // PWM active, but the down voltage does not reach -12V
  hal.pilot_mock_down_voltage_n12 = false;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_DIODE_SHORT_BIT & evse.get_error());

  // auto clear after 1 min, EV still connected: E -> A -> B1
  hal.step(61000);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
}

TEST(charging_current_and_max) {
  ev_connect_sequence();
  TEST_ASSERT_EQUAL(320, hal.pilot_mock_amps);

  // change charging current while PWM is active
  TEST_ASSERT_TRUE(evse.set_charging_current(160));
  TEST_ASSERT_EQUAL(160, hal.pilot_mock_amps);

  // out of range
  TEST_ASSERT_FALSE(evse.set_charging_current(59));
  TEST_ASSERT_FALSE(evse.set_charging_current(321));
  TEST_ASSERT_EQUAL(160, evse.get_charging_current());

  // lowering the max clamps the charging current and the PWM
  TEST_ASSERT_TRUE(evse.set_max_charging_current(10));
  TEST_ASSERT_EQUAL(100, evse.get_charging_current());
  TEST_ASSERT_EQUAL(100, hal.pilot_mock_amps);
  TEST_ASSERT_FALSE(evse.set_max_charging_current(5));
  TEST_ASSERT_FALSE(evse.set_max_charging_current(64));
  TEST_ASSERT_EQUAL(10, evse.get_max_charging_current());

  // raising the max does not change the charging current
  TEST_ASSERT_TRUE(evse.set_max_charging_current(32));
  TEST_ASSERT_EQUAL(100, evse.get_charging_current());
}

TEST(rcm_selftest_async_pass) {
  evse.set_rcm(true);
  hal.rcm_mock_result = RcmSelftestResult::PENDING;

  // EV connect: A -> B1, self test started
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_EQUAL(1, hal.rcm_selftest_starts);
  TEST_ASSERT_TRUE(evse.is_rcm_selftest_pending());
  int trigger_calls = hal.rcm_triggered_calls;

  // charging not allowed while self test is pending
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  hal.pilot_mock_up_voltage = PilotVoltage::V6;
  hal.pilot_mock_down_voltage_n12 = true;
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_EQUAL(trigger_calls, hal.rcm_triggered_calls);  // no trigger check during the self test

  // self test passed: C1 -> C2
  hal.rcm_mock_result = RcmSelftestResult::PASSED;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_rcm_selftest_pending());

  // self test is performed only once per session
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.set_enabled(false);
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_EQUAL(1, hal.rcm_selftest_starts);
}

TEST(rcm_selftest_fail) {
  evse.set_rcm(true);
  hal.rcm_mock_result = RcmSelftestResult::FAILED;

  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_RCM_SELFTEST_FAULT_BIT & evse.get_error());

  // auto clear after 1 min, new self test in the new session
  hal.rcm_mock_result = RcmSelftestResult::PASSED;
  hal.step(61000);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_EQUAL(2, hal.rcm_selftest_starts);
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
}

TEST(rcm_selftest_cancel_on_disconnect) {
  evse.set_rcm(true);
  hal.rcm_mock_result = RcmSelftestResult::PENDING;

  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  TEST_ASSERT_TRUE(evse.is_rcm_selftest_pending());

  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_FALSE(evse.is_rcm_selftest_pending());
  TEST_ASSERT_EQUAL(1, hal.rcm_selftest_cancels);
}

TEST(rcm_triggered) {
  evse.set_rcm(true);
  ev_connect_sequence();
  ev_charge_sequence();

  hal.rcm_mock_triggered = true;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_RCM_TRIGGERED_BIT & evse.get_error());

  // auto clear after 1 min
  hal.step(30000);
  evse.process();
  TEST_ASSERT_EQUAL(EvseState::E, evse.get_state());
  hal.step(31000);
  evse.process();
  TEST_ASSERT_EQUAL(EvseState::A, evse.get_state());
  TEST_ASSERT_EQUAL(0, evse.get_error());
}

TEST(rcm_disabled_ignores_trigger) {
  ev_connect_sequence();
  hal.pilot_mock_down_voltage_n12 = true;
  hal.rcm_mock_triggered = true;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
  TEST_ASSERT_EQUAL(0, hal.rcm_triggered_calls);
  TEST_ASSERT_EQUAL(0, hal.rcm_selftest_starts);
}

TEST(rcm_not_configured_no_rcm_at_all) {
  // like esp32-evse on a board without RCM: no self test, no trigger check, no waiting
  hal.rcm_mock_triggered = true;
  hal.rcm_mock_result = RcmSelftestResult::FAILED;

  ev_connect_sequence();  // B1 -> B2 right in the next iteration
  ev_charge_sequence();
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  ev_connect_sequence();
  hal.pilot_mock_down_voltage_n12 = true;
  evse.process();

  TEST_ASSERT_FALSE(evse.is_rcm());
  TEST_ASSERT_FALSE(evse.is_rcm_selftest_pending());
  TEST_ASSERT_EQUAL(0, hal.rcm_selftest_starts);
  TEST_ASSERT_EQUAL(0, hal.rcm_selftest_polls);
  TEST_ASSERT_EQUAL(0, hal.rcm_selftest_cancels);
  TEST_ASSERT_EQUAL(0, hal.rcm_triggered_calls);
  TEST_ASSERT_EQUAL(0, evse.get_error());
}

TEST(temperature_high_and_fault) {
  evse.set_temperature_monitoring(true);
  TEST_ASSERT_TRUE(evse.set_temp_threshold(60));
  TEST_ASSERT_FALSE(evse.set_temp_threshold(39));
  TEST_ASSERT_FALSE(evse.set_temp_threshold(81));

  ev_connect_sequence();
  ev_charge_sequence();

  // high temperature: C2 -> E
  hal.temp_mock_valid = true;
  hal.temp_mock_high = 61.0f;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_TEMPERATURE_HIGH_BIT & evse.get_error());

  // temperature high is not cleared automatically
  hal.step(120000);
  evse.process();
  TEST_ASSERT_EQUAL(EvseState::E, evse.get_state());

  // without PWM the EV opened S2 (state B), then the temperature is back to normal:
  // E -> A (the state machine continues in the next iteration)
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  hal.temp_mock_high = 50.0f;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);

  // sensor fault
  hal.temp_mock_error = true;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_TEMPERATURE_FAULT_BIT & evse.get_error());
  hal.temp_mock_error = false;
  evse.process();
  TEST_ASSERT_EQUAL(EvseState::A, evse.get_state());
}

TEST(error_clear_restarts_from_a) {
  // esp32-evse resumed C2 (relay on) when a non auto-clear error cleared
  evse.set_temperature_monitoring(true);
  ev_connect_sequence();
  ev_charge_sequence();

  hal.temp_mock_valid = true;
  hal.temp_mock_high = 70.0f;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);

  // EV unplugged during the error, then the temperature drops
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  hal.temp_mock_high = 30.0f;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
}

TEST(temperature_without_valid_value) {
  evse.set_temperature_monitoring(true);
  hal.temp_mock_valid = false;
  hal.temp_mock_high = 100.0f;
  ev_connect_sequence();
  TEST_ASSERT_EQUAL(0, evse.get_error());
}

TEST(auth_grant_before_connect) {
  evse.set_require_auth(true);
  evse.authorize();

  // EV connects within the grant time
  hal.step(30000);
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
  TEST_ASSERT_FALSE(evse.is_pending_auth());
}

TEST(auth_grant_expired) {
  evse.set_require_auth(true);
  evse.authorize();

  hal.step(61000);
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_TRUE(evse.is_pending_auth());
}

TEST(auth_custom_timeout) {
  evse.set_require_auth(true);
  evse.set_auth_timeout(10000);
  evse.authorize();

  hal.step(11000);
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  evse.process();
  TEST_ASSERT_TRUE(evse.is_pending_auth());
}

TEST(auth_source_active) {
  evse.set_require_auth(true);

  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  evse.process();
  TEST_ASSERT_TRUE(evse.is_pending_auth());

  // level of the authorization source authorizes the session
  hal.auth_mock_active = true;
  hal.pilot_mock_down_voltage_n12 = true;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  // deactivating the source does not end an authorized session
  hal.auth_mock_active = false;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  // a new session requires a new authorization
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_TRUE(evse.is_pending_auth());
}

TEST(auth_consumed_by_session) {
  evse.set_require_auth(true);
  evse.authorize();

  ev_connect_sequence();  // consumes the grant

  hal.pilot_mock_down_voltage_n12 = true;
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);

  // reconnect within the original grant window still requires a new authorization
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_TRUE(evse.is_pending_auth());
}

TEST(no_meter_ignores_power_limits) {
  hal.energy_meter_mock_available = false;
  evse.set_under_power_limit(1000);
  evse.set_consumption_limit(1);
  hal.energy_meter_mock_consumption = 100;

  ev_connect_sequence();
  ev_charge_sequence();
  hal.step(120000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());
}

// Stay below the limit for more than UNDER_POWER_TIME
static void under_power_wait(int minutes = 3) {
  for (int i = 0; i < minutes; i++) {
    hal.step(61000);
    evse.process();
  }
}

// The under power limit applies only after the power was above the limit in the session
TEST(under_power_needs_power_above_limit) {
  evse.set_under_power_limit(1000);
  ev_connect_sequence();
  ev_charge_sequence();

  // below the limit from the beginning of the session: not reached
  hal.energy_meter_mock_power = 0;
  under_power_wait(10);
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  hal.energy_meter_mock_power = 999;
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // the limit itself is not above the limit
  hal.energy_meter_mock_power = 1000;
  evse.process();
  hal.energy_meter_mock_power = 999;
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());

  // above the limit, then below it: reached after 1 min
  hal.energy_meter_mock_power = 1001;
  evse.process();
  hal.energy_meter_mock_power = 999;
  evse.process();  // timer starts
  hal.step(59000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  hal.step(2000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
  TEST_ASSERT_EQUAL(LIMIT_UNDER_POWER_BIT, evse.get_reached_limit());
}

// The time under the limit starts again when charging starts. In esp32-evse it kept running
// while the vehicle paused, so the limit was reached right after the vehicle resumed.
TEST(under_power_time_restarts_with_charging) {
  evse.set_under_power_limit(1000);
  ev_connect_sequence();
  ev_charge_sequence();
  hal.energy_meter_mock_power = 7000;
  evse.process();

  // the vehicle reduces the power and pauses 5 s later: C2 -> B2
  hal.energy_meter_mock_power = 0;
  evse.process();  // timer starts
  hal.step(5000);
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);

  // it resumes 10 min later: B2 -> C2, it has 1 min to ramp up
  hal.step(600000);
  evse.process();
  hal.pilot_mock_up_voltage = PilotVoltage::V6;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  evse.process();  // timer starts
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());
  hal.step(59000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  hal.energy_meter_mock_power = 7000;
  evse.process();
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());

  // pause and resume again, the power stays below the limit: reached 1 min after charging started
  hal.energy_meter_mock_power = 0;
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
  hal.step(600000);
  evse.process();
  hal.pilot_mock_up_voltage = PilotVoltage::V6;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  evse.process();  // timer starts
  hal.step(59000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  hal.step(2000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
  TEST_ASSERT_TRUE(evse.is_limit_reached());
}

// A reached under power limit stops charging until the end of the session
TEST(under_power_reached_until_session_end) {
  evse.set_under_power_limit(1000);
  ev_connect_sequence();
  ev_charge_sequence();
  hal.energy_meter_mock_power = 7000;
  evse.process();
  hal.energy_meter_mock_power = 500;
  evse.process();  // timer starts
  hal.step(61000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);

  // the vehicle stops: C1 -> B1, and stays there
  hal.energy_meter_mock_power = 0;
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  under_power_wait();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_TRUE(evse.is_limit_reached());

  // new session: the power has to get above the limit again
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_FALSE(evse.is_limit_reached());
  ev_connect_sequence();
  ev_charge_sequence();
  hal.energy_meter_mock_power = 500;
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());
}

// The highest power of the session is compared with the actual limit
TEST(under_power_limit_changed_in_session) {
  // no limit yet
  ev_connect_sequence();
  ev_charge_sequence();
  hal.energy_meter_mock_power = 7000;
  evse.process();
  hal.energy_meter_mock_power = 3000;
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);

  // limit above the highest power of the session: it does not apply
  evse.set_under_power_limit(8000);
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());

  // limit below it: the time starts when the limit is set
  evse.set_under_power_limit(5000);
  evse.process();  // timer starts
  hal.step(59000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  hal.step(2000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
}

TEST(under_power_unknown_power) {
  evse.set_under_power_limit(1000);
  ev_connect_sequence();
  ev_charge_sequence();

  // the meter has no valid value: it does not count as power above the limit
  hal.energy_meter_mock_power = POWER_UNKNOWN;
  under_power_wait();
  hal.energy_meter_mock_power = 500;
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());

  // and not as power below the limit
  hal.energy_meter_mock_power = 7000;
  evse.process();
  hal.energy_meter_mock_power = POWER_UNKNOWN;
  under_power_wait();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  TEST_ASSERT_FALSE(evse.is_limit_reached());

  // valid again, below the limit: reached after 1 min
  hal.energy_meter_mock_power = 500;
  evse.process();
  hal.step(61000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
}

TEST(under_power_reset_by_authorize) {
  evse.set_under_power_limit(1000);
  ev_connect_sequence();
  ev_charge_sequence();
  hal.energy_meter_mock_power = 7000;
  evse.process();

  hal.energy_meter_mock_power = 500;
  evse.process();  // timer starts
  hal.step(50000);
  evse.authorize();  // restarts the timer
  evse.process();
  hal.step(50000);
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
  hal.step(11000);
  evse.process();
  ASSERT_EVSE(EvseState::C1, PILOT_MOCK_STATE_HIGH, true);
}

TEST(d_state_ventilation) {
  ev_connect_sequence();
  hal.pilot_mock_up_voltage = PilotVoltage::V3;
  hal.pilot_mock_down_voltage_n12 = true;
  evse.process();
  // B2 only knows 12/9/6V, 3V is a pilot fault from B2
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
}

TEST(d2_from_c2) {
  ev_connect_sequence();
  ev_charge_sequence();
  hal.pilot_mock_up_voltage = PilotVoltage::V3;
  evse.process();
  ASSERT_EVSE(EvseState::D2, PILOT_MOCK_STATE_PWM, true);
  hal.pilot_mock_up_voltage = PilotVoltage::V6;
  evse.process();
  ASSERT_EVSE(EvseState::C2, PILOT_MOCK_STATE_PWM, true);
}

TEST(clear_error_fault_gone) {
  hal.pilot_mock_up_voltage = PilotVoltage::V1;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);

  // fault removed, cleared manually without waiting 1 min: E -> A
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  TEST_ASSERT_TRUE(evse.clear_error());
  TEST_ASSERT_EQUAL(0, evse.get_error());
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
}

TEST(clear_error_fault_persists) {
  hal.pilot_mock_up_voltage = PilotVoltage::V1;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);

  // the fault is still there: E -> A -> E
  TEST_ASSERT_TRUE(evse.clear_error());
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_PILOT_FAULT_BIT & evse.get_error());

  // the auto clear waits 1 min again from the new error
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  hal.step(30000);
  evse.process();
  TEST_ASSERT_EQUAL(EvseState::E, evse.get_state());
  hal.step(31000);
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
}

TEST(clear_error_reconnected_vehicle) {
  ev_connect_sequence();
  hal.pilot_mock_down_voltage_n12 = false;  // diode short
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);

  // vehicle still connected: E -> A -> B1 -> B2, a new session
  hal.pilot_mock_down_voltage_n12 = true;
  TEST_ASSERT_TRUE(evse.clear_error());
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
  TEST_ASSERT_EQUAL(2, hal.session_starts);
}

TEST(clear_error_rcm_selftest_again) {
  evse.set_rcm(true);
  ev_connect_sequence();
  ev_charge_sequence();
  hal.rcm_mock_triggered = true;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_RCM_TRIGGERED_BIT & evse.get_error());
  TEST_ASSERT_EQUAL(1, hal.rcm_selftest_starts);

  // after clearing, the RCM is tested again before charging
  TEST_ASSERT_TRUE(evse.clear_error());
  hal.pilot_mock_up_voltage = PilotVoltage::V9;
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  evse.process();
  ASSERT_EVSE(EvseState::B1, PILOT_MOCK_STATE_HIGH, false);
  TEST_ASSERT_EQUAL(2, hal.rcm_selftest_starts);
}

TEST(clear_error_keeps_temperature_errors) {
  evse.set_temperature_monitoring(true);
  hal.temp_mock_valid = true;
  hal.temp_mock_high = 70.0f;
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);

  TEST_ASSERT_FALSE(evse.clear_error());
  evse.process();
  ASSERT_EVSE(EvseState::E, PILOT_MOCK_STATE_LOW, false);
  TEST_ASSERT_TRUE(ERR_TEMPERATURE_HIGH_BIT & evse.get_error());

  // with a pilot fault too, only the pilot fault is cleared
  hal.pilot_mock_up_voltage = PilotVoltage::V1;
  hal.temp_mock_high = 50.0f;
  evse.process();  // temperature cleared, A
  evse.process();  // pilot fault
  hal.temp_mock_high = 70.0f;
  evse.process();
  TEST_ASSERT_EQUAL(ERR_PILOT_FAULT_BIT | ERR_TEMPERATURE_HIGH_BIT, evse.get_error());
  hal.pilot_mock_up_voltage = PilotVoltage::V12;
  TEST_ASSERT_TRUE(evse.clear_error());
  evse.process();
  TEST_ASSERT_EQUAL(ERR_TEMPERATURE_HIGH_BIT, evse.get_error());
  TEST_ASSERT_EQUAL(EvseState::E, evse.get_state());
}

TEST(clear_error_without_error) {
  TEST_ASSERT_FALSE(evse.clear_error());
  evse.process();
  ASSERT_EVSE(EvseState::A, PILOT_MOCK_STATE_HIGH, false);
  ev_connect_sequence();
  TEST_ASSERT_FALSE(evse.clear_error());
  hal.pilot_mock_down_voltage_n12 = true;
  evse.process();
  ASSERT_EVSE(EvseState::B2, PILOT_MOCK_STATE_PWM, false);
}

TEST(state_strings) {
  TEST_ASSERT_TRUE(std::string("A") == evse_state_to_str(EvseState::A));
  TEST_ASSERT_TRUE(std::string("B1") == evse_state_to_str(EvseState::B1));
  TEST_ASSERT_TRUE(std::string("C2") == evse_state_to_str(EvseState::C2));
  TEST_ASSERT_TRUE(std::string("D2") == evse_state_to_str(EvseState::D2));
  TEST_ASSERT_TRUE(std::string("E") == evse_state_to_str(EvseState::E));
  TEST_ASSERT_TRUE(std::string("F") == evse_state_to_str(EvseState::F));
}

// ---------------------------------------------------------------------------

int main() {
  int run = 0;
  for (auto &t : tests()) {
    g_current_test = t.name;
    std::printf("TEST %s\n", t.name);
    try {
      setup();
      t.fn();
    } catch (int) {
      // failure already reported
    }
    run++;
  }
  std::printf("\n%d tests, %d failures\n", run, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
