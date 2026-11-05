#pragma once

// Test only: a simulated vehicle and the analog front end of the ESP32-S2-DA board,
// to run the evse and evse_energy_meter components on the ESPHome host platform.
//
// * PilotOutput replaces the ledc output of the pilot.
// * PilotSampler returns the ADC voltage of the pilot feedback: the EV state (A/B/C/D)
//   sets the positive voltage (12/9/6/3V), the negative half is -12V; the PWM phase is
//   derived from micros() (1kHz).
// * WaveSampler returns 50Hz sine waves of the current / voltage inputs. The EV draws
//   the current advertised by the PWM in state C/D while the relay is closed.

#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/voltage_sampler/voltage_sampler.h"

namespace esphome {
namespace ev_simulator {

class PilotOutput : public output::FloatOutput {
 public:
  float get_level() const { return this->level_; }

 protected:
  void write_state(float state) override { this->level_ = state; }
  float level_{0.0f};
};

class EvSimulator : public Component {
 public:
  void set_relay(switch_::Switch *relay) { this->relay_ = relay; }
  void set_pilot_output(PilotOutput *output) { this->pilot_output_ = output; }

  /// 'A' (disconnected), 'B' (connected), 'C' (charging), 'D' (charging with ventilation)
  void set_ev_state(char state);
  char get_ev_state() const { return this->ev_state_; }

  /// Pilot voltage at this moment
  float pilot_voltage() const;
  /// Current drawn per phase in A
  float ev_current() const;
  float grid_voltage() const { return 230.0f; }

 protected:
  switch_::Switch *relay_{nullptr};
  PilotOutput *pilot_output_{nullptr};
  char ev_state_{'A'};
};

class PilotSampler : public voltage_sampler::VoltageSampler, public Parented<EvSimulator> {
 public:
  float sample() override;
};

class WaveSampler : public voltage_sampler::VoltageSampler, public Parented<EvSimulator> {
 public:
  void set_input(bool is_current, uint8_t phase) {
    this->is_current_ = is_current;
    this->phase_ = phase;
  }
  float sample() override;

 protected:
  bool is_current_{true};
  uint8_t phase_{0};
};

}  // namespace ev_simulator
}  // namespace esphome
