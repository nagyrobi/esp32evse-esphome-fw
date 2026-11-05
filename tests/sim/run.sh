#!/usr/bin/env bash
# Runs the EVSE components on the ESPHome host platform with a simulated vehicle
# (sim.yaml) and checks the log. Requires esphome and a native C++ compiler.
set -euo pipefail

cd "$(dirname "$0")"
LOG="${TMPDIR:-/tmp}/evse-sim.log"

esphome compile sim.yaml
# the scenario takes ~45s
timeout 50 stdbuf -oL -eL .esphome/build/evse-sim/.pioenvs/evse-sim/program > "$LOG" 2>&1 || true
python3 check_log.py "$LOG"
