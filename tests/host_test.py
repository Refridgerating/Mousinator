#!/usr/bin/env python3
"""Focused host-side tests for M3 motor-test driver preparation."""

from __future__ import annotations

import sys
import types
import unittest
from pathlib import Path

try:
    import serial  # noqa: F401
except ImportError:
    serial_module = types.ModuleType("serial")
    serial_module.SerialException = OSError
    tools_module = types.ModuleType("serial.tools")
    list_ports_module = types.ModuleType("serial.tools.list_ports")
    list_ports_module.comports = lambda: []
    tools_module.list_ports = list_ports_module
    sys.modules["serial"] = serial_module
    sys.modules["serial.tools"] = tools_module
    sys.modules["serial.tools.list_ports"] = list_ports_module

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from host.sentry_mcu import (  # noqa: E402
    AxisState,
    DriverDiagnostics,
    DriverStatus,
    EndstopState,
    SentryMcu,
    prepare_axis_motor_test,
    prepare_pan_motor_test,
    query_driver_report,
)


def status(axis: str, *, configured: bool, error: str = "NONE") -> DriverStatus:
    return DriverStatus(
        axis=axis.upper(),
        address=2 if axis.lower() == "pan" else 1,
        present=True,
        configured=configured,
        error=error,
        interface_count=8,
        ioin=0x21000000,
        gstat=0,
        drv_status=0,
        run_current_ma=520 if configured else 0,
        hold_current_ma=275 if configured else 0,
        microsteps=16 if configured else 0,
        interpolate=configured,
        mode="STEALTHCHOP" if configured else "UNCONFIGURED",
        overtemperature_warning=False,
        standstill=True,
        stealthchop_active=configured,
        fatal=False,
    )


class FakeController:
    def __init__(self, pan_statuses, tilt_status, configure_response):
        self.pan_statuses = iter(pan_statuses)
        self.tilt_status = tilt_status
        self.configure_response = configure_response
        self.commands = []

    def driver(self, axis):
        if axis == "pan":
            return next(self.pan_statuses)
        return self.tilt_status

    def command(self, command):
        self.commands.append(command)
        return self.configure_response


class MotorTestPreparationTests(unittest.TestCase):
    def test_ready_pan_skips_configuration(self):
        controller = FakeController(
            [status("pan", configured=True)],
            status("tilt", configured=True),
            "unused",
        )
        prepare_pan_motor_test(controller, output=lambda value: None)
        self.assertEqual(controller.commands, [])

    def test_partial_tilt_failure_can_continue_with_ready_pan(self):
        output = []
        controller = FakeController(
            [
                status("pan", configured=False),
                status("pan", configured=True),
            ],
            status("tilt", configured=False, error="TIMEOUT"),
            "ERR DRIVER_CONFIG PAN=CONFIGURED TILT=ERROR",
        )
        result = prepare_pan_motor_test(controller, output=output.append)
        self.assertTrue(result.configured)
        self.assertEqual(controller.commands, ["DRIVER CONFIGURE"])
        self.assertTrue(any(str(value).startswith("WARNING:") for value in output))

    def test_pan_still_unready_aborts_before_enable(self):
        controller = FakeController(
            [
                status("pan", configured=False),
                status("pan", configured=False, error="TIMEOUT"),
            ],
            status("tilt", configured=True),
            "ERR DRIVER_CONFIG PAN=ERROR TILT=CONFIGURED",
        )
        with self.assertRaises(RuntimeError):
            prepare_pan_motor_test(controller, output=lambda value: None)

    def test_tilt_only_depends_on_tilt_readiness(self):
        class TiltController:
            def __init__(self):
                self.tilt_statuses = iter(
                    [
                        status("tilt", configured=False),
                        status("tilt", configured=True),
                    ]
                )
                self.commands = []

            def driver(self, axis):
                if axis == "tilt":
                    return next(self.tilt_statuses)
                return status("pan", configured=False, error="TIMEOUT")

            def command(self, command):
                self.commands.append(command)
                return "ERR DRIVER_CONFIG PAN=ERROR TILT=CONFIGURED"

        controller = TiltController()
        result = prepare_axis_motor_test(
            controller, "tilt", output=lambda value: None
        )
        self.assertTrue(result.configured)
        self.assertEqual(controller.commands, ["DRIVER CONFIGURE"])


class DriverDiagnosticTests(unittest.TestCase):
    RESPONSE = (
        "OK DRIVER_DIAG TILT ADDR=1 UART_ORE=2 UART_NE=4 UART_FE=1 "
        "UART_PE=0 UART_LAST_ADDR=1 UART_LAST_OP=READ UART_LAST_REG=0x06 "
        "UART_FLAGS=0x04 UART_RETRIES=7 LAST_OP=READ LAST_REG=0x6F "
        "LAST_ATTEMPT=2 TRANSPORT=UART PARSER=NONE "
        "LAST_CFG_STAGE=CONFIG_PWMCONF LAST_CFG_REG=0x70 "
        "LAST_CFG_PHASE=READBACK_READ LAST_CFG_ERR=UART"
    )

    def test_immutable_diagnostic_parser(self):
        controller = SentryMcu.__new__(SentryMcu)
        commands = []

        def expect_ok(command):
            commands.append(command)
            return self.RESPONSE

        controller.expect_ok = expect_ok
        diagnostics = controller.driver_diagnostics("tilt")
        self.assertIsInstance(diagnostics, DriverDiagnostics)
        self.assertEqual(commands, ["DRIVER-DIAG? TILT"])
        self.assertEqual(diagnostics.uart_noise_count, 4)
        self.assertEqual(diagnostics.uart_last_address, 1)
        self.assertEqual(diagnostics.last_register, 0x6F)
        self.assertEqual(diagnostics.configuration_stage, "CONFIG_PWMCONF")
        with self.assertRaises(Exception):
            diagnostics.uart_noise_count = 0

    def test_driver_report_orders_live_status_before_snapshot(self):
        calls = []
        expected_status = status("tilt", configured=False, error="UART")
        expected_diagnostics = object()

        class ReportController:
            def driver(self, axis):
                calls.append(("status", axis))
                return expected_status

            def driver_diagnostics(self, axis):
                calls.append(("diagnostics", axis))
                return expected_diagnostics

        report = query_driver_report(ReportController(), "tilt")
        self.assertEqual(report, (expected_status, expected_diagnostics))
        self.assertEqual(calls, [("status", "tilt"), ("diagnostics", "tilt")])


class M4ParsingTests(unittest.TestCase):
    TILT_STATE = (
        "OK TILT ENABLED=1 DISABLING=0 MOVING=0 POS=50 VEL=0 TARGET=0 "
        "TIMEOUT=0 HOMED=1 HOMING=0 HOME_STATUS=SUCCESS JOGGING=0 "
        "DIR_CHECKING=0 DIR_CALIBRATED=1 MIN_LIMIT=0 "
        "MAX_CONFIGURED=0 MAX_LIMIT=0"
    )

    def test_tilt_state_parser(self):
        controller = SentryMcu.__new__(SentryMcu)
        commands = []

        def expect_ok(command):
            commands.append(command)
            return self.TILT_STATE

        controller.expect_ok = expect_ok
        parsed = controller.state("tilt")
        self.assertIsInstance(parsed, AxisState)
        self.assertEqual(commands, ["STATE? TILT"])
        self.assertTrue(parsed.homed)
        self.assertTrue(parsed.direction_calibrated)
        self.assertEqual(parsed.position_steps, 50)

    def test_endstop_parser_is_read_only_query(self):
        controller = SentryMcu.__new__(SentryMcu)
        commands = []

        def expect_ok(command):
            commands.append(command)
            return "OK TILT ENDSTOP=1 RAW=0"

        controller.expect_ok = expect_ok
        parsed = controller.endstop()
        self.assertIsInstance(parsed, EndstopState)
        self.assertTrue(parsed.triggered)
        self.assertFalse(parsed.raw_high)
        self.assertEqual(commands, ["ENDSTOP? TILT"])


if __name__ == "__main__":
    unittest.main()
