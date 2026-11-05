#!/usr/bin/env bash
# Build and run the host unit tests of the EVSE state machine.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/evse_host_tests"
mkdir -p "$OUT"

CXX="${CXX:-g++}"
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DEVSE_HOST_TEST \
  -I"$ROOT/components/evse" \
  "$ROOT/tests/host/test_evse_core.cpp" \
  "$ROOT/components/evse/evse_core.cpp" \
  -o "$OUT/test_evse_core"

if [ "${VERBOSE:-0}" = "1" ]; then
  "$OUT/test_evse_core"
else
  # state machine logs are noisy, show only the test results
  "$OUT/test_evse_core" | grep -E "^(TEST|  FAIL|[0-9]+ tests)"
fi
