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
PAN_STATE_PATTERN = re.compile(
    r"^OK PAN ENABLED=(?P<enabled>[01]) DISABLING=(?P<disabling>[01]) "
    r"MOVING=(?P<moving>[01]) POS=(?P<position>-?\d+) "
    r"VEL=(?P<velocity>-?\d+) TARGET=(?P<target>-?\d+) "
    r"TIMEOUT=(?P<timeout>[01])$"
)
TILT_STATE_PATTERN = re.compile(
    r"^OK TILT ENABLED=(?P<enabled>[01]) DISABLING=(?P<disabling>[01]) "
    r"MOVING=(?P<moving>[01]) POS=(?P<position>-?\d+) "
    r"VEL=(?P<velocity>-?\d+) TARGET=(?P<target>-?\d+) "
    r"TIMEOUT=(?P<timeout>[01]) HOMED=(?P<homed>[01]) "
    r"HOMING=(?P<homing>[01]) HOME_STATUS=(?P<home_status>[A-Z_]+) "
    r"JOGGING=(?P<jogging>[01]) DIR_CHECKING=(?P<direction_checking>[01]) "
    r"DIR_CALIBRATED=(?P<direction_calibrated>[01]) "
    r"MIN_LIMIT=(?P<min_limit>[01]) MAX_CONFIGURED=0 MAX_LIMIT=0$"
)
ENDSTOP_PATTERN = re.compile(
    r"^OK TILT ENDSTOP=(?P<triggered>[01]) RAW=(?P<raw>[01])$"
)
DRIVER_PATTERN = re.compile(
    r"^OK DRIVER (?P<axis>PAN|TILT) ADDR=(?P<address>\d+) "
    r"PRESENT=(?P<present>[01]) CONFIGURED=(?P<configured>[01]) "
    r"ERR=(?P<error>[A-Z_]+) IFCNT=(?P<ifcnt>\d+) "
    r"IOIN=(?P<ioin>0x[0-9A-F]{8}) GSTAT=(?P<gstat>0x[0-9A-F]{8}) "
    r"DRV=(?P<drv>0x[0-9A-F]{8}) RUN_MA=(?P<run_ma>\d+) "
    r"HOLD_MA=(?P<hold_ma>\d+) MSTEP=(?P<microsteps>\d+) "
    r"INTPOL=(?P<interpolate>[01]) "
    r"MODE=(?P<mode>STEALTHCHOP|UNCONFIGURED) OTPW=(?P<otpw>[01]) "
    r"STST=(?P<standstill>[01]) STEALTH=(?P<stealth_active>[01]) "
    r"FATAL=(?P<fatal>[01])$"
)
DRIVER_DIAGNOSTIC_PATTERN = re.compile(
    r"^OK DRIVER_DIAG (?P<axis>PAN|TILT) ADDR=(?P<address>\d+) "
    r"UART_ORE=(?P<uart_ore>\d+) UART_NE=(?P<uart_ne>\d+) "
    r"UART_FE=(?P<uart_fe>\d+) UART_PE=(?P<uart_pe>\d+) "
    r"UART_LAST_ADDR=(?P<uart_last_address>NONE|\d+) "
    r"UART_LAST_OP=(?P<uart_last_operation>NONE|READ|WRITE) "
    r"UART_LAST_REG=(?P<uart_last_register>0x[0-9A-F]{2}) "
    r"UART_FLAGS=(?P<uart_flags>0x[0-9A-F]{2}) "
    r"UART_RETRIES=(?P<uart_retries>\d+) "
    r"LAST_OP=(?P<last_operation>NONE|READ|WRITE) "
    r"LAST_REG=(?P<last_register>0x[0-9A-F]{2}) "
    r"LAST_ATTEMPT=(?P<last_attempt>\d+) "
    r"TRANSPORT=(?P<transport_error>[A-Z_]+) "
    r"PARSER=(?P<parser_error>[A-Z_]+) "
    r"LAST_CFG_STAGE=(?P<configuration_stage>[A-Z_]+) "
    r"LAST_CFG_REG=(?P<configuration_register>0x[0-9A-F]{2}) "
    r"LAST_CFG_PHASE=(?P<configuration_phase>[A-Z_]+) "
    r"LAST_CFG_ERR=(?P<configuration_error>[A-Z_]+)$"
)


@dataclass(frozen=True)
class AxisState:
    axis: str
    enabled: bool
    disabling: bool
    moving: bool
    position_steps: int
    velocity_steps_s: int
    target_velocity_steps_s: int
    motion_timeout: bool
    homed: bool = False
    homing: bool = False
    home_status: str = "IDLE"
    jogging: bool = False
    direction_checking: bool = False
    direction_calibrated: bool = False
    min_limit: bool = False


@dataclass(frozen=True)
class EndstopState:
    triggered: bool
    raw_high: bool


@dataclass(frozen=True)
class DriverStatus:
    axis: str
    address: int
    present: bool
    configured: bool
    error: str
    interface_count: int
    ioin: int
    gstat: int
    drv_status: int
    run_current_ma: int
    hold_current_ma: int
    microsteps: int
    interpolate: bool
    mode: str
    overtemperature_warning: bool
    standstill: bool
    stealthchop_active: bool
    fatal: bool


@dataclass(frozen=True)
class DriverDiagnostics:
    axis: str
    address: int
    uart_overrun_count: int
    uart_noise_count: int
    uart_framing_count: int
    uart_parity_count: int
    uart_last_address: int | None
    uart_last_operation: str
    uart_last_register: int
    uart_flags: int
    uart_retry_count: int
    last_operation: str
    last_register: int
    last_attempt: int
    transport_error: str
    parser_error: str
    configuration_stage: str
    configuration_register: int
    configuration_phase: str
    configuration_error: str


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

    def state(self, axis: str = "pan") -> AxisState:
        normalized_axis = axis.upper()
        if normalized_axis not in ("PAN", "TILT"):
            raise ValueError("state axis must be pan or tilt")
        command = "STATE?" if normalized_axis == "PAN" else "STATE? TILT"
        response = self.expect_ok(command)
        match = (
            PAN_STATE_PATTERN if normalized_axis == "PAN" else TILT_STATE_PATTERN
        ).fullmatch(response)
        if match is None:
            raise RuntimeError(f"unexpected {command} response: {response}")
        fields = match.groupdict()
        return AxisState(
            axis=normalized_axis,
            enabled=fields["enabled"] == "1",
            disabling=fields["disabling"] == "1",
            moving=fields["moving"] == "1",
            position_steps=int(fields["position"]),
            velocity_steps_s=int(fields["velocity"]),
            target_velocity_steps_s=int(fields["target"]),
            motion_timeout=fields["timeout"] == "1",
            homed=fields.get("homed") == "1",
            homing=fields.get("homing") == "1",
            home_status=fields.get("home_status", "IDLE"),
            jogging=fields.get("jogging") == "1",
            direction_checking=fields.get("direction_checking") == "1",
            direction_calibrated=fields.get("direction_calibrated") == "1",
            min_limit=fields.get("min_limit") == "1",
        )

    def endstop(self) -> EndstopState:
        response = self.expect_ok("ENDSTOP? TILT")
        match = ENDSTOP_PATTERN.fullmatch(response)
        if match is None:
            raise RuntimeError(f"unexpected ENDSTOP? response: {response}")
        return EndstopState(
            triggered=match.group("triggered") == "1",
            raw_high=match.group("raw") == "1",
        )

    def driver(self, axis: str) -> DriverStatus:
        normalized_axis = axis.upper()
        if normalized_axis not in ("PAN", "TILT"):
            raise ValueError("driver axis must be pan or tilt")
        response = self.expect_ok(f"DRIVER? {normalized_axis}")
        match = DRIVER_PATTERN.fullmatch(response)
        if match is None:
            raise RuntimeError(f"unexpected DRIVER? response: {response}")
        fields = match.groupdict()
        return DriverStatus(
            axis=fields["axis"],
            address=int(fields["address"]),
            present=fields["present"] == "1",
            configured=fields["configured"] == "1",
            error=fields["error"],
            interface_count=int(fields["ifcnt"]),
            ioin=int(fields["ioin"], 16),
            gstat=int(fields["gstat"], 16),
            drv_status=int(fields["drv"], 16),
            run_current_ma=int(fields["run_ma"]),
            hold_current_ma=int(fields["hold_ma"]),
            microsteps=int(fields["microsteps"]),
            interpolate=fields["interpolate"] == "1",
            mode=fields["mode"],
            overtemperature_warning=fields["otpw"] == "1",
            standstill=fields["standstill"] == "1",
            stealthchop_active=fields["stealth_active"] == "1",
            fatal=fields["fatal"] == "1",
        )

    def configure_drivers(self) -> str:
        return self.expect_ok("DRIVER CONFIGURE")

    def driver_diagnostics(self, axis: str) -> DriverDiagnostics:
        normalized_axis = axis.upper()
        if normalized_axis not in ("PAN", "TILT"):
            raise ValueError("driver axis must be pan or tilt")
        response = self.expect_ok(f"DRIVER-DIAG? {normalized_axis}")
        match = DRIVER_DIAGNOSTIC_PATTERN.fullmatch(response)
        if match is None:
            raise RuntimeError(
                f"unexpected DRIVER-DIAG? response: {response}"
            )
        fields = match.groupdict()
        last_address = fields["uart_last_address"]
        return DriverDiagnostics(
            axis=fields["axis"],
            address=int(fields["address"]),
            uart_overrun_count=int(fields["uart_ore"]),
            uart_noise_count=int(fields["uart_ne"]),
            uart_framing_count=int(fields["uart_fe"]),
            uart_parity_count=int(fields["uart_pe"]),
            uart_last_address=(
                None if last_address == "NONE" else int(last_address)
            ),
            uart_last_operation=fields["uart_last_operation"],
            uart_last_register=int(fields["uart_last_register"], 16),
            uart_flags=int(fields["uart_flags"], 16),
            uart_retry_count=int(fields["uart_retries"]),
            last_operation=fields["last_operation"],
            last_register=int(fields["last_register"], 16),
            last_attempt=int(fields["last_attempt"]),
            transport_error=fields["transport_error"],
            parser_error=fields["parser_error"],
            configuration_stage=fields["configuration_stage"],
            configuration_register=int(fields["configuration_register"], 16),
            configuration_phase=fields["configuration_phase"],
            configuration_error=fields["configuration_error"],
        )


def driver_is_ready(status: DriverStatus) -> bool:
    return (
        status.present
        and status.configured
        and status.error == "NONE"
        and not status.fatal
    )


def query_driver_report(
    controller: SentryMcu, axis: str
) -> tuple[DriverStatus, DriverDiagnostics]:
    status = controller.driver(axis)
    diagnostics = controller.driver_diagnostics(axis)
    return status, diagnostics


def prepare_axis_motor_test(
    controller: SentryMcu, axis: str, output=print
) -> DriverStatus:
    normalized_axis = axis.lower()
    status = controller.driver(normalized_axis)
    output(status)
    configuration_response: str | None = None

    if not driver_is_ready(status):
        configuration_response = controller.command("DRIVER CONFIGURE")
        output(configuration_response)
        # DRIVER CONFIGURE addresses both devices, but the requested axis is
        # authoritative for a single-axis test.
        status = controller.driver(normalized_axis)
        output(status)
        if not driver_is_ready(status):
            raise RuntimeError(
                f"{normalized_axis.upper()} driver is not configured and fault-free"
            )

    other_axis = "tilt" if normalized_axis == "pan" else "pan"
    other = controller.driver(other_axis)
    output(other)
    if not driver_is_ready(other) or (
        configuration_response is not None
        and configuration_response.startswith("ERR DRIVER_CONFIG")
    ):
        output(
            f"WARNING: {other_axis.upper()} driver unavailable; "
            f"proceeding with {normalized_axis.upper()}-only test"
        )
    return status


def prepare_pan_motor_test(controller: SentryMcu, output=print) -> DriverStatus:
    """Backward-compatible name retained for focused M3 host tests."""
    return prepare_axis_motor_test(controller, "pan", output)


def wait_for_stop(
    controller: SentryMcu, axis: str = "pan", timeout_s: float = 3.0
) -> AxisState:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = controller.state(axis)
        if (
            not state.moving
            and state.velocity_steps_s == 0
            and not state.jogging
            and not state.direction_checking
        ):
            return state
        time.sleep(0.05)
    raise TimeoutError(
        f"{axis.upper()} did not reach zero velocity before validation timeout"
    )


def wait_for_home(controller: SentryMcu, timeout_s: float = 15.0) -> AxisState:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = controller.state("tilt")
        if not state.homing:
            if state.homed and state.home_status == "SUCCESS":
                return state
            raise RuntimeError(f"TILT homing failed: {state.home_status}")
        time.sleep(0.05)
    raise TimeoutError("TILT homing did not finish before validation timeout")


def run_segment(
    controller: SentryMcu,
    axis: str,
    velocity: int,
    duration_s: float,
    refresh_s: float,
) -> None:
    deadline = time.monotonic() + duration_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            break
        print(controller.expect_ok(f"VEL {axis.upper()} {velocity}"))
        time.sleep(min(refresh_s, remaining))
    print(controller.expect_ok("STOP"))
    print(controller.state(axis))
    print(wait_for_stop(controller, axis))


def run_motor_test(
    controller: SentryMcu,
    axis: str,
    velocity: int,
    duration_s: float,
    refresh_s: float,
) -> None:
    if not 1 <= velocity <= 1000:
        raise ValueError("motor-test velocity must be in the range 1..1000")
    if duration_s <= 0.0:
        raise ValueError("motor-test duration must be positive")
    if not 0.0 < refresh_s < 1.0:
        raise ValueError("motor-test refresh must be greater than 0 and less than 1 s")

    print(controller.expect_ok("PING"))
    print(controller.expect_ok("INFO"))
    normalized_axis = axis.lower()
    prepare_axis_motor_test(controller, normalized_axis)
    if normalized_axis == "tilt":
        state = controller.state("tilt")
        if not state.direction_calibrated:
            raise RuntimeError(
                "TILT direction is not calibrated; run ENDSTOP and "
                "DIR-CHECK commissioning first"
            )
        print("WARNING: TILT has no positive software maximum; use short travel")
    print(controller.expect_ok(f"ENABLE {normalized_axis.upper()}"))
    try:
        if normalized_axis == "tilt":
            print(controller.expect_ok("HOME TILT"))
            print(wait_for_home(controller))
        run_segment(controller, normalized_axis, velocity, duration_s, refresh_s)
        run_segment(controller, normalized_axis, -velocity, duration_s, refresh_s)
    finally:
        try:
            print(controller.expect_ok("STOP"))
            print(wait_for_stop(controller, normalized_axis))
        finally:
            print(controller.expect_ok(f"DISABLE {normalized_axis.upper()}"))
            print(controller.state(normalized_axis))


def run_dual_segment(
    controller: SentryMcu,
    pan_velocity: int,
    tilt_velocity: int,
    duration_s: float,
    refresh_s: float,
) -> None:
    deadline = time.monotonic() + duration_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            break
        print(
            controller.expect_ok(f"VEL BOTH {pan_velocity} {tilt_velocity}")
        )
        time.sleep(min(refresh_s, remaining))
    print(controller.expect_ok("STOP"))
    print(wait_for_stop(controller, "pan"))
    print(wait_for_stop(controller, "tilt"))


def run_dual_motor_test(
    controller: SentryMcu,
    pan_velocity: int,
    tilt_velocity: int,
    duration_s: float,
    refresh_s: float,
) -> None:
    for value, name in ((pan_velocity, "PAN"), (tilt_velocity, "TILT")):
        if not 1 <= value <= 1000:
            raise ValueError(f"{name} velocity must be in the range 1..1000")
    if duration_s <= 0.0 or not 0.0 < refresh_s < 1.0:
        raise ValueError("duration/refresh values are outside safe bounds")

    print(controller.expect_ok("PING"))
    print(controller.expect_ok("INFO"))
    prepare_axis_motor_test(controller, "pan")
    prepare_axis_motor_test(controller, "tilt")
    state = controller.state("tilt")
    if not state.direction_calibrated:
        raise RuntimeError("TILT direction is not calibrated")
    print(controller.expect_ok("ENABLE PAN"))
    print(controller.expect_ok("ENABLE TILT"))
    try:
        print("WARNING: TILT has no positive software maximum; use short travel")
        print(controller.expect_ok("HOME TILT"))
        print(wait_for_home(controller))
        run_dual_segment(
            controller, pan_velocity, tilt_velocity, duration_s, refresh_s
        )
        run_dual_segment(
            controller, -pan_velocity, -tilt_velocity, duration_s, refresh_s
        )
    finally:
        cleanup_errors: list[str] = []
        for command in ("STOP", "DISABLE PAN", "DISABLE TILT"):
            try:
                print(controller.command(command))
            except Exception as exc:  # cleanup must continue for the other axis
                cleanup_errors.append(f"{command}: {exc}")
        print(controller.state("pan"))
        print(controller.state("tilt"))
        if cleanup_errors:
            raise RuntimeError("cleanup failures: " + "; ".join(cleanup_errors))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="CDC device; otherwise discover 1209:0001")
    subparsers = parser.add_subparsers(dest="action", required=True)

    for action in ("ping", "info", "stop", "configure-drivers"):
        subparsers.add_parser(action)

    for action in ("state", "enable", "disable"):
        axis_parser = subparsers.add_parser(action)
        axis_parser.add_argument("--axis", choices=("pan", "tilt"), default="pan")

    driver_parser = subparsers.add_parser("driver")
    driver_parser.add_argument("axis", choices=("pan", "tilt"))

    velocity_parser = subparsers.add_parser("velocity")
    velocity_parser.add_argument("steps_per_second", type=int)
    velocity_parser.add_argument(
        "--axis", choices=("pan", "tilt"), default="pan"
    )

    motor_parser = subparsers.add_parser(
        "motor-test", help="run an explicit, brief two-direction motor validation"
    )
    motor_parser.add_argument("--axis", choices=("pan", "tilt"), default="pan")
    motor_parser.add_argument("--velocity", type=int, default=150)
    motor_parser.add_argument("--duration", type=float, default=0.5)
    motor_parser.add_argument("--refresh", type=float, default=0.2)

    dual_parser = subparsers.add_parser(
        "dual-motor-test", help="run bounded simultaneous PAN/TILT validation"
    )
    dual_parser.add_argument("--pan-velocity", type=int, default=300)
    dual_parser.add_argument("--tilt-velocity", type=int, default=200)
    dual_parser.add_argument("--duration", type=float, default=1.0)
    dual_parser.add_argument("--refresh", type=float, default=0.2)

    subparsers.add_parser("endstop")
    subparsers.add_parser("home-tilt")
    direction_parser = subparsers.add_parser("tilt-dir-check")
    direction_parser.add_argument("--level", choices=("high", "low"), required=True)
    jog_parser = subparsers.add_parser("jog-tilt")
    jog_parser.add_argument("--steps", type=int, default=50)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        device = args.device or discover_device()
        print(f"Using {device}")
        with SentryMcu(device) as controller:
            if args.action == "motor-test":
                run_motor_test(
                    controller,
                    args.axis,
                    args.velocity,
                    args.duration,
                    args.refresh,
                )
            elif args.action == "dual-motor-test":
                run_dual_motor_test(
                    controller,
                    args.pan_velocity,
                    args.tilt_velocity,
                    args.duration,
                    args.refresh,
                )
            elif args.action == "state":
                print(controller.state(args.axis))
            elif args.action == "driver":
                status, diagnostics = query_driver_report(controller, args.axis)
                print(status)
                print(diagnostics)
            elif args.action == "configure-drivers":
                print(controller.configure_drivers())
            elif args.action == "endstop":
                print(controller.endstop())
            elif args.action == "home-tilt":
                prepare_axis_motor_test(controller, "tilt")
                state = controller.state("tilt")
                if not state.direction_calibrated:
                    raise RuntimeError("TILT direction is not calibrated")
                print(controller.endstop())
                print(controller.expect_ok("HOME TILT"))
                print(wait_for_home(controller))
            elif args.action == "tilt-dir-check":
                prepare_axis_motor_test(controller, "tilt")
                endstop = controller.endstop()
                print(endstop)
                if endstop.triggered:
                    raise RuntimeError(
                        "release the TILT endstop before direction checking"
                    )
                print(controller.expect_ok(f"DIR-CHECK TILT {args.level.upper()}"))
                print(wait_for_stop(controller, "tilt"))
            elif args.action == "jog-tilt":
                print(controller.expect_ok(f"JOG TILT {args.steps}"))
                print(wait_for_stop(controller, "tilt"))
            else:
                commands = {
                    "ping": "PING",
                    "info": "INFO",
                    "stop": "STOP",
                }
                if args.action == "velocity":
                    command = f"VEL {args.axis.upper()} {args.steps_per_second}"
                elif args.action == "enable":
                    command = f"ENABLE {args.axis.upper()}"
                elif args.action == "disable":
                    command = f"DISABLE {args.axis.upper()}"
                else:
                    command = commands[args.action]
                print(controller.command(command))
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError, serial.SerialException) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
