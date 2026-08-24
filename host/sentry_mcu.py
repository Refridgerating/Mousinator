#!/usr/bin/env python3
"""Minimal Raspberry Pi validation client for Sentry MCU firmware."""

from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - depends on the Pi environment
    raise SystemExit("pyserial is required: python3 -m pip install pyserial") from exc


SENTRY_VID = 0x1209
SENTRY_PID = 0x0001
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT_S = 1.0
STATE_PATTERN = re.compile(
    r"^OK PAN ENABLED=(?P<enabled>[01]) DISABLING=(?P<disabling>[01]) "
    r"MOVING=(?P<moving>[01]) POS=(?P<position>-?\d+) "
    r"VEL=(?P<velocity>-?\d+) TARGET=(?P<target>-?\d+) "
    r"TIMEOUT=(?P<timeout>[01])$"
)


@dataclass(frozen=True)
class PanState:
    enabled: bool
    disabling: bool
    moving: bool
    position_steps: int
    velocity_steps_s: int
    target_velocity_steps_s: int
    motion_timeout: bool


def discover_device() -> str:
    matches = [
        port.device
        for port in list_ports.comports()
        if port.vid == SENTRY_VID and port.pid == SENTRY_PID
    ]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise RuntimeError(
            "no Sentry CDC device found; pass --device /dev/ttyACM<number>"
        )
    raise RuntimeError(
        "multiple Sentry CDC devices found; pass --device explicitly: "
        + ", ".join(matches)
    )


class SentryMcu:
    def __init__(self, device: str, timeout_s: float = DEFAULT_TIMEOUT_S) -> None:
        self._serial = serial.Serial(
            port=device,
            baudrate=DEFAULT_BAUD,
            timeout=timeout_s,
            write_timeout=timeout_s,
        )
        self._serial.reset_input_buffer()

    def close(self) -> None:
        self._serial.close()

    def __enter__(self) -> "SentryMcu":
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()

    def command(self, command: str) -> str:
        payload = (command + "\r\n").encode("ascii")
        self._serial.write(payload)
        self._serial.flush()
        response = self._serial.readline()
        if not response:
            raise TimeoutError(f"no response to {command!r}")
        return response.decode("ascii", errors="strict").strip()

    def expect_ok(self, command: str) -> str:
        response = self.command(command)
        if not response.startswith("OK"):
            raise RuntimeError(f"{command!r} failed: {response}")
        return response

    def state(self) -> PanState:
        response = self.expect_ok("STATE?")
        match = STATE_PATTERN.fullmatch(response)
        if match is None:
            raise RuntimeError(f"unexpected STATE? response: {response}")
        fields = match.groupdict()
        return PanState(
            enabled=fields["enabled"] == "1",
            disabling=fields["disabling"] == "1",
            moving=fields["moving"] == "1",
            position_steps=int(fields["position"]),
            velocity_steps_s=int(fields["velocity"]),
            target_velocity_steps_s=int(fields["target"]),
            motion_timeout=fields["timeout"] == "1",
        )


def wait_for_stop(controller: SentryMcu, timeout_s: float = 2.0) -> PanState:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = controller.state()
        if not state.moving and state.velocity_steps_s == 0:
            return state
        time.sleep(0.05)
    raise TimeoutError("PAN did not reach zero velocity before validation timeout")


def run_segment(
    controller: SentryMcu, velocity: int, duration_s: float, refresh_s: float
) -> None:
    deadline = time.monotonic() + duration_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            break
        print(controller.expect_ok(f"VEL PAN {velocity}"))
        time.sleep(min(refresh_s, remaining))
    print(controller.expect_ok("STOP"))
    print(controller.state())
    print(wait_for_stop(controller))


def run_motor_test(
    controller: SentryMcu, velocity: int, duration_s: float, refresh_s: float
) -> None:
    if not 1 <= velocity <= 1000:
        raise ValueError("motor-test velocity must be in the range 1..1000")
    if duration_s <= 0.0:
        raise ValueError("motor-test duration must be positive")
    if not 0.0 < refresh_s < 1.0:
        raise ValueError("motor-test refresh must be greater than 0 and less than 1 s")

    print(controller.expect_ok("PING"))
    print(controller.expect_ok("INFO"))
    print(controller.expect_ok("ENABLE PAN"))
    try:
        run_segment(controller, velocity, duration_s, refresh_s)
        run_segment(controller, -velocity, duration_s, refresh_s)
    finally:
        try:
            print(controller.expect_ok("STOP"))
            print(wait_for_stop(controller))
        finally:
            print(controller.expect_ok("DISABLE PAN"))
            print(controller.state())


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="CDC device; otherwise discover 1209:0001")
    subparsers = parser.add_subparsers(dest="action", required=True)

    for action in ("ping", "info", "state", "enable", "stop", "disable"):
        subparsers.add_parser(action)

    velocity_parser = subparsers.add_parser("velocity")
    velocity_parser.add_argument("steps_per_second", type=int)

    motor_parser = subparsers.add_parser(
        "motor-test", help="run an explicit, brief two-direction PAN validation"
    )
    motor_parser.add_argument("--velocity", type=int, default=150)
    motor_parser.add_argument("--duration", type=float, default=0.5)
    motor_parser.add_argument("--refresh", type=float, default=0.2)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        device = args.device or discover_device()
        print(f"Using {device}")
        with SentryMcu(device) as controller:
            if args.action == "motor-test":
                run_motor_test(
                    controller, args.velocity, args.duration, args.refresh
                )
            elif args.action == "state":
                print(controller.state())
            else:
                commands = {
                    "ping": "PING",
                    "info": "INFO",
                    "enable": "ENABLE PAN",
                    "stop": "STOP",
                    "disable": "DISABLE PAN",
                }
                if args.action == "velocity":
                    command = f"VEL PAN {args.steps_per_second}"
                else:
                    command = commands[args.action]
                print(controller.command(command))
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError, serial.SerialException) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
