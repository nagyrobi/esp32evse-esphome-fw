"""Checks the log of the simulation scenario of sim.yaml."""

from pathlib import Path
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def main(path: str) -> int:
    with Path(path).open(encoding="utf-8", errors="replace") as file:
        lines = [ANSI.sub("", line.rstrip()) for line in file]

    failures = []

    def check(condition: bool, message: str) -> None:
        print(("ok   " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)

    # the expected events, in this order
    expected = [
        "SCENARIO: EV connects",
        "Enter B1 state",
        "Start session",
        "SCENARIO: authorize",
        "Authorized",
        "Enter B2 state",
        "Set pilot PWM 32.0A, duty 53.33%",
        "SCENARIO: EV requests charging",
        "Enter C2 state",
        "Set AC relay ON",
        "Charging started",
        "SCENARIO: charging current 10A",
        "Set pilot PWM 10.0A, duty 16.67%",
        "SCENARIO: disable charging",
        "Enter C1 state",
        "Set pilot level +12V",
        "Force switch off ac relay",
        "Set AC relay OFF",
        "SCENARIO: enable charging",
        "Enter C2 state",
        "Set AC relay ON",
        "SCENARIO: EV stops charging",
        "Enter B2 state",
        "Set AC relay OFF",
        "SCENARIO: EV disconnects",
        "Enter A state",
        "Stop session",
        "SCENARIO: temperature high",
        "Enter E state",
        "Set pilot level -12V",
        "LED error ON",
        "SCENARIO: temperature normal",
        "Enter A state",
        "LED error off",
        "SCENARIO: END",
    ]
    position = 0
    for event in expected:
        index = next(
            (i for i in range(position, len(lines)) if event in lines[i]), None
        )
        check(index is not None, f"event in order: {event}")
        if index is None:
            break
        position = index + 1

    # no unexpected state changes before the first connect
    first_connect = next(
        (i for i, line in enumerate(lines) if "SCENARIO: EV connects" in line), 0
    )
    check(
        not any(
            "Enter " in line and " state" in line for line in lines[:first_connect]
        ),
        "stays in state A until the vehicle connects",
    )

    # relay switched only in C2 (and off by the force switch off / B2 / A)
    relay_on = [i for i, line in enumerate(lines) if "Set AC relay ON" in line]
    check(len(relay_on) == 2, f"relay switched on twice (got {len(relay_on)})")

    # session statistics: 6s at 32A and 10s at 10A on 3 x 230V
    match = next(
        (
            re.search(
                r"Stop session, energy (\d+)Wh, session time (\d+)s, charging time (\d+)s",
                line,
            )
            for line in lines
            if "Stop session" in line
        ),
        None,
    )
    check(match is not None, "session statistics logged")
    if match:
        energy, session_time, charging_time = (int(v) for v in match.groups())
        expected_energy = (22080 * 6 + 6900 * 10) / 3600  # 56Wh
        check(
            abs(energy - expected_energy) <= 4,
            f"session energy {energy}Wh ~ {expected_energy:.0f}Wh",
        )
        check(29 <= session_time <= 31, f"session time {session_time}s ~ 30s")
        check(15 <= charging_time <= 17, f"charging time {charging_time}s ~ 16s")

    powers = [
        float(m.group(1))
        for line in lines
        if (m := re.search(r"power (\d+)W", line)) and "evse_energy_meter" in line
    ]
    check(any(abs(p - 22080) < 100 for p in powers), "measured 22080W at 32A")
    check(any(abs(p - 6900) < 50 for p in powers), "measured 6900W at 10A")

    errors = [line for line in lines if "[E]" in line]
    check(not errors, "no errors" + (f": {errors[:3]}" if errors else ""))

    # informational only: on the host delayMicroseconds() is a nanosleep with ~60us
    # granularity, so timing is not representative of the ESP32
    for line in lines:
        if "took a long time" in line:
            print(f"info {line}")

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
