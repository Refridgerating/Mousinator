# AGENTS.md

## Purpose

This repository contains the custom embedded firmware for the Sentry motion-control subsystem.

The immediate target hardware is:

- BIGTREETECH SKR Mini E3 V2.0
- STM32F103RC MCU
- Integrated TMC2209 stepper drivers
- Raspberry Pi host connected over USB
- Two primary motion axes:
  - YM connector / Y driver = pan
  - ZAM connector / Z driver = tilt

ZAM and ZBM are parallel outputs from the same Z driver. They are not
independent axes and must never be controlled as separate motors.

The firmware converts high-level motion commands from the Raspberry Pi into deterministic stepper motion.

The Raspberry Pi performs perception, object detection, tracking, target selection, and high-level control.

The STM32 performs real-time motor control, step generation, local safety handling, motor-driver configuration, and hardware state reporting.

Do not expand this repository into the rest of the Sentry system.

---

# Current Scope

Develop only the SKR Mini E3 V2.0 firmware and the minimum host-side utilities required to test it.

Primary responsibilities:

1. STM32 startup and board initialization
2. Safe GPIO initialization
3. USB CDC communication
4. Command parsing
5. Step and direction generation
6. Pan and tilt motor control
7. TMC2209 UART configuration
8. Endstop or Hall sensor inputs
9. Motion limits
10. Acceleration and deceleration control
11. Communication watchdog
12. Fault detection
13. Telemetry
14. Firmware build support
15. Minimal host test utilities

Do not implement:

- YOLO
- camera handling
- object detection
- object tracking
- web interfaces
- Moonraker
- Klipper host software
- Marlin
- printer functions
- slicer integration
- heaters
- extrusion
- bed control
- autonomous target-selection logic
- Sentry application UI

---

# Architecture

The intended control hierarchy is:

```text
Raspberry Pi
    |
    | high-level USB commands
    v
STM32F103RC
    |
    | deterministic real-time control
    v
TMC2209 drivers
    |
    v
Pan / Tilt stepper motors
```

The Raspberry Pi should send commands such as desired position, velocity, stop state, or configuration.

The Raspberry Pi must never generate individual STEP pulses.

All timing-critical step generation belongs on the STM32.

---

# Design Philosophy

Prefer a small, auditable embedded firmware over adapting 3D-printer firmware.

Avoid unnecessary frameworks and abstractions.

Current preferred implementation:

- C
- bare-metal or lightweight STM32 support
- libopencm3 where practical
- hardware timers for motion generation
- interrupts only where they provide clear deterministic behavior

Do not introduce:

- RTOS dependencies unless demonstrated to be necessary
- dynamic memory allocation in normal runtime operation
- large third-party frameworks
- Arduino abstractions
- printer-specific firmware components

Prefer deterministic behavior, explicit state machines, fixed-size buffers, and bounded execution time.

---

# Bootloader and Flash Layout

The existing BIGTREETECH SD-card bootloader must be preserved.

Do not overwrite or replace the original BTT bootloader.

The SKR Mini E3 V2.0 uses a 28 KiB bootloader.

Application firmware must therefore begin at:

```text
0x08007000
```

The vector table must be relocated accordingly.

Expected flash layout:

```text
0x08000000
+--------------------------+
| BTT bootloader           |
| approximately 28 KiB     |
+--------------------------+
0x08007000
| Sentry firmware          |
|                          |
+--------------------------+
```

Build output must include:

```text
firmware.bin
```

The current supported flashing workflow is:

1. Build `firmware.bin`
2. Copy it to the root of a FAT32 microSD card
3. Insert card into the SKR
4. Power-cycle the SKR
5. Allow the existing BTT bootloader to flash the application

Do not implement a custom USB bootloader at this stage.

USB firmware update support may be investigated later.

---

# Hardware Mapping

Target board:

```text
BIGTREETECH SKR Mini E3 V2.0
STM32F103RC
```

Sentry motor assignments:

```text
Y driver / YM  = PAN
Z driver / ZAM = TILT
```

Known primary pin mapping:

## Pan / Y / YM

```text
STEP      PB10
DIR       PB2
ENABLE    PB11
```

ENABLE is active low.

## Tilt / Z / ZAM

```text
STEP      PB0
DIR       PC5
ENABLE    PB1
```

ENABLE is active low.

TILT motion is not implemented yet. ZAM and ZBM are wired in parallel to this
single Z driver and must not be treated as separate logical axes. Logical
reference/Hall sensor assignments have not yet been established.

## TMC2209 UART

```text
TX        PC10
RX        PC11
```

The TMC2209 UART bus is shared.

Initial driver addresses:

```text
Y / PAN   address 2
Z / TILT  address 1
```

Verify all board mappings against authoritative SKR Mini E3 V2.0 documentation or known-good upstream board definitions before changing hardware-control code.

Never infer an MCU pin assignment from another SKR revision.

---

# Unused Printer Outputs

This board was originally designed for a 3D printer.

Sentry does not use heater, extruder, or printer fan functionality.

All unused high-current outputs must be explicitly placed into safe OFF states during startup.

Known outputs include printer heater and fan MOSFETs.

These outputs must never float or be unintentionally enabled.

At boot:

1. Configure relevant GPIO immediately
2. Drive all unused power outputs OFF
3. Disable all stepper drivers
4. Initialize communication
5. Only enable motors after an explicit host command

Safety-critical GPIO initialization should happen as early as practical.

---

# Motor Safety

Motors must default to disabled.

No motor may move solely because the MCU booted.

Motion requires an explicit valid command.

The following conditions must force commanded velocity toward zero or disable motion as appropriate:

- USB communication timeout
- invalid command state
- internal firmware fault
- limit violation
- emergency stop command
- unrecoverable TMC driver fault

A host disconnect must never leave a motor executing indefinitely.

Implement a communication watchdog.

Initial target behavior:

```text
No valid host command for 250 ms
        |
        v
commanded velocity -> 0
        |
        v
controlled deceleration
```

The timeout should be configurable later.

Do not immediately hard-disable a moving stepper unless necessary for safety.

Prefer controlled deceleration when possible.

---

# Motion Model

The firmware is primarily a pan/tilt motion controller, not a printer trajectory planner.

The main runtime interface should support velocity-oriented tracking.

Conceptually:

```text
Host target velocity
       |
       v
Acceleration limiter
       |
       v
Current axis velocity
       |
       v
Timer-based step generator
```

Position tracking should remain available internally.

Maintain signed integer step position for every axis.

Preferred internal representation:

```text
position_steps
velocity_steps_per_second
target_velocity_steps_per_second
acceleration_steps_per_second_squared
```

Do not make floating-point math part of interrupt-time step generation unless measurements demonstrate that it is acceptable.

Prefer fixed-point or integer representations for timing-critical code.

Conversions between:

- degrees
- steps
- radians
- host-facing engineering units

should occur outside the tightest real-time path.

---

# Step Generation

STEP pulses must be generated by STM32 hardware timers or deterministic timer-driven logic.

Do not generate motor pulses using:

- blocking delays
- sleep loops
- USB timing
- main-loop polling alone

Requirements:

- deterministic pulse timing
- minimum valid STEP pulse width
- safe direction-change timing
- support positive and negative velocity
- bounded acceleration
- zero-velocity state without timer instability

Timer architecture must be documented.

Before supporting both axes, prove correct operation on a single axis.

---

# TMC2209

The firmware should eventually configure the integrated TMC2209 drivers directly over UART.

Initial capabilities should include:

- communication verification
- driver identity/status read
- motor current configuration
- microstep configuration
- StealthChop configuration where appropriate
- fault reporting
- enable/disable behavior

Do not depend on hard-coded undocumented register values.

Create a small TMC2209 driver layer with named registers and bit fields.

Keep motor-driver control separate from motion planning.

---

# USB Communication

Initial host communication should use USB CDC.

Human-readable commands are preferred during early development because they simplify debugging.

The first firmware should support:

```text
PING
INFO
```

Example:

```text
> PING
< OK PONG
```

```text
> INFO
< OK SENTRY-MCU 0.2.1 SKR_MINI_E3_V2
```

Later commands may include:

```text
ENABLE PAN
ENABLE TILT
ENABLE ALL

DISABLE PAN
DISABLE TILT
DISABLE ALL

VEL <pan> <tilt>

STOP

STATE?
POS?
FAULT?
```

Exact syntax may evolve.

Keep protocol parsing bounded and deterministic.

Never use an unbounded input buffer.

Reject malformed input cleanly.

Every command should produce either:

```text
OK ...
```

or:

```text
ERR ...
```

where practical.

Do not silently ignore invalid commands.

---

# Protocol Evolution

Do not optimize prematurely.

Use text USB CDC commands until measurements show that protocol overhead is materially limiting control performance.

USB bandwidth is expected to be far greater than required for pan/tilt tracking.

A binary protocol may be introduced later if justified by:

- measured latency
- framing reliability
- telemetry volume
- synchronization requirements

Do not introduce a binary protocol merely because it appears more embedded-oriented.

---

# Firmware State Machine

Prefer explicit states.

Suggested top-level states:

```text
BOOT
SAFE
READY
ACTIVE
FAULT
ESTOP
```

Example transition:

```text
BOOT
 |
 v
SAFE
 |
 | initialization successful
 v
READY
 |
 | explicit enable
 v
ACTIVE
```

Any serious fault:

```text
ACTIVE -> FAULT
```

Emergency stop:

```text
ANY STATE -> ESTOP
```

State transitions should be centralized and testable.

---

# Fault Model

Create explicit fault codes.

Potential faults include:

```text
FAULT_NONE
FAULT_COMM_TIMEOUT
FAULT_TMC_UART
FAULT_TMC_DRIVER
FAULT_LIMIT
FAULT_PROTOCOL
FAULT_INTERNAL
FAULT_WATCHDOG
```

Avoid representing fault state only through log strings.

Maintain a machine-readable fault enum or bitmask internally.

---

# Endstops and Hall Sensors

Initial hardware may use Hall sensors instead of mechanical endstops.

Endstop inputs must therefore be abstracted as axis reference sensors rather than assuming mechanical switches.

Required capabilities:

- digital input
- configurable active polarity
- debounce
- state reporting
- optional homing behavior

Pan may eventually support continuous rotation and may not require a hard travel endstop.

Tilt is expected to require bounded travel.

Do not assume identical mechanical constraints between pan and tilt.

---

# Pan Axis

Pan may ultimately support continuous 360-degree or multi-turn rotation.

Do not hard-code printer-style linear travel limits into the low-level architecture.

The firmware should support both:

```text
bounded axis
```

and:

```text
continuous axis
```

behavior.

Continuous rotation creates an important distinction between:

```text
absolute accumulated position
```

and:

```text
wrapped angular position
```

Keep internal motor position unwrapped.

Angular wrapping should occur only at higher abstraction levels where needed.

---

# Tilt Axis

Tilt is expected to have physical travel limits.

The firmware must support:

- configurable minimum position
- configurable maximum position
- reference or home sensor
- prevention of commanded motion beyond limits

Software limits must not be trusted until the axis reference position is known.

---

# Real-Time Constraints

Do not perform the following inside high-frequency interrupts:

- USB parsing
- formatted printing
- heap allocation
- complex telemetry formatting
- slow UART transactions
- blocking calls

Interrupt handlers should be short and deterministic.

Use shared state carefully.

Variables shared between interrupt and main contexts must use appropriate synchronization or atomic access patterns.

Use `volatile` only where semantically appropriate. Do not treat `volatile` as a substitute for synchronization.

---

# Memory

The STM32F103RC has limited embedded RAM compared with the Raspberry Pi.

Prefer:

- static allocation
- fixed-size buffers
- explicit bounds
- small structs
- no unnecessary copies

Avoid:

```c
malloc()
calloc()
realloc()
free()
```

in runtime firmware unless there is a compelling documented reason.

---

# Logging

USB logging is useful during development but must not interfere with motion timing.

Support log levels conceptually:

```text
ERROR
WARN
INFO
DEBUG
```

Production motion must remain functional with logging disabled.

Never print directly from timing-critical interrupts.

---

# Coding Style

Use clear C.

Prefer explicit names over compressed embedded shorthand.

Examples:

```c
pan_target_velocity
tilt_position_steps
motion_watchdog_timeout_ms
tmc2209_set_current()
```

Prefer this over:

```c
pv
tp
wdt
seti()
```

Use:

```text
snake_case
```

for functions and variables.

Use:

```text
UPPER_SNAKE_CASE
```

for constants and macros.

Use fixed-width integer types where representation matters:

```c
uint8_t
uint16_t
uint32_t
int32_t
```

Keep modules focused.

Suggested boundaries:

```text
board.*
usb.*
protocol.*
motion.*
stepper.*
tmc2209.*
endstop.*
fault.*
```

Do not create abstraction layers with only one trivial caller unless they improve hardware isolation or testability.

---

# Comments

Comments should explain:

- hardware constraints
- timing constraints
- non-obvious register behavior
- why a design decision exists

Do not comment obvious C syntax.

Good:

```c
// BTT bootloader occupies the first 28 KiB.
// Application vectors therefore begin at 0x08007000.
```

Bad:

```c
// Increment i
i++;
```

---

# Documentation

Document any hardware assumption that could damage hardware if incorrect.

For every MCU peripheral added, document:

- peripheral
- pins
- clock
- interrupt
- timer channel if applicable
- reason it was selected

Maintain a concise architecture description in the repository.

---

# Build System

Provide a reproducible command-line build.

Preferred interface:

```bash
make
```

Expected output:

```text
build/sentry.elf
build/sentry.bin
firmware.bin
```

The build should fail loudly for:

- missing toolchain
- unsupported target
- linker overflow
- compile warnings promoted to errors where reasonable

Keep build dependencies small.

---

# Compiler Quality

Compile with strong warnings.

At minimum investigate:

```text
-Wall
-Wextra
-Wshadow
-Wconversion
-Wundef
```

Do not blindly enable warnings that produce large amounts of meaningless noise.

New code should compile without unexplained warnings.

Do not silence warnings globally just to obtain a clean build.

---

# Development Sequence

Follow this sequence unless a compelling reason requires changing it.

## M0: Board Bring-Up

Implement:

- linker script
- startup
- vector relocation
- clock initialization
- safe GPIO initialization
- motor drivers disabled
- unused power outputs OFF

Acceptance:

- firmware boots repeatedly
- no motors move
- no high-current outputs activate
- MCU remains stable

## M1: USB CDC

Implement:

- USB CDC enumeration
- bounded RX buffer
- simple command parser
- `PING`
- `INFO`

Acceptance:

```text
PING -> OK PONG
```

from Raspberry Pi over USB.

## M2: Single-Axis Motion

Current implementation decision:

- TIM3 provides a 1 kHz control tick for integer acceleration, deadman timing,
  and a phase accumulator.
- TIM2_CH3 with partial-remap 2 generates 2 us one-shot PAN STEP pulses on
  PB10 at 1 us timer resolution. TIM2_CH4 remains disabled so PB11 remains an
  ordinary active-low enable GPIO.
- M2 supports signed raw velocities through 1000 steps/s, uses 2000 steps/s²,
  and a 1000 ms velocity-command lease.
- Reversal passes through zero before PB2 changes. A moving disable is
  asynchronous and completes only after controlled deceleration.
- M0 and M1 are hardware-validated. M2 is build-verified but must remain
  hardware-unvalidated until the physical motor acceptance test passes.

Use PAN through the Y driver and YM connector first.

Implement:

- STEP
- DIR
- ENABLE
- timer-driven pulse generation
- commanded velocity
- controlled stop

Acceptance:

- motor enables explicitly
- rotates both directions
- velocity is repeatable
- STOP decelerates safely
- USB traffic does not disturb pulse timing

## M3: TMC2209

Implement:

- UART communication
- register reads
- current configuration
- microsteps
- driver status

Acceptance:

- firmware confirms communication with PAN driver
- driver configuration can be read back
- current can be set predictably

## M4: Two-Axis Motion

Implement:

- pan
- tilt
- simultaneous motion
- independent velocities
- acceleration limits

Acceptance:

- both motors operate concurrently
- no timer interference
- no USB-induced motion jitter

## M5: Sensors and Safety

Implement:

- endstop/Hall inputs
- debounce
- software limits
- host watchdog
- fault states
- telemetry

Acceptance:

- communication loss stops motion safely
- reference input is detected reliably
- bounded axis cannot intentionally exceed configured limits

## M6: Host Test Utility

Provide a minimal Python utility for:

- finding the controller
- PING
- INFO
- enable
- velocity
- stop
- state query

This utility exists for firmware testing only.

It must not become the full Sentry control application.

---

# Testing Strategy

Prefer tests at multiple levels.

## Host-Compiled Unit Tests

Where logic can be separated from MCU peripherals, test it on the development machine.

Candidates:

- command parser
- motion-state transitions
- limit handling
- watchdog logic
- unit conversion
- fault-state logic

## Hardware Tests

Keep tests intentionally bounded.

Early motor tests should use:

- low current
- low speed
- low acceleration
- short movement duration
- easy access to power disconnect

Never introduce a startup test that automatically moves motors.

---

# Safety Rules for Codex

Before modifying hardware-control code, inspect existing pin definitions and board documentation.

Do not guess at:

- GPIO assignments
- GPIO polarity
- timer channels
- TMC addresses
- bootloader offsets
- flash layout

Do not enable heater MOSFETs.

Do not implement automatic motor movement on startup.

Do not remove communication-loss handling once implemented.

Do not replace the BTT bootloader.

Do not modify unrelated Sentry repositories or host application code.

Do not add printer functionality merely because upstream examples contain it.

When adapting code from Marlin, Klipper, libopencm3 examples, STM32 examples, or BTT sources, extract only the hardware information or minimal concepts required.

Avoid importing large printer-specific subsystems.

---

# Scope Discipline

The current objective is not to build the entire final controller immediately.

The first meaningful target is:

```text
SKR boots custom Sentry firmware
        +
Pi recognizes USB CDC device
        +
PING returns OK PONG
```

Nothing else should block that milestone.

After it succeeds, implement one motor axis.

Do not simultaneously develop:

- two-axis control
- homing
- telemetry
- TMC tuning
- binary protocol
- tracking integration

before basic board bring-up and USB communication are verified.

---

# Repository Philosophy

This firmware should remain small enough that its critical behavior can be understood by reading the source.

A useful target is:

```text
simple
deterministic
hardware-specific
auditable
recoverable
```

The SKR Mini E3 V2.0 is being treated as a general-purpose STM32 motion-control board.

It should not be treated as a 3D printer motherboard at the software architecture level.

---

# Immediate Task

M0 and M1 have been physically validated. Unless explicitly instructed
otherwise, Codex should work toward physical validation and correction of:

```text
Milestone M2
```

Do not begin TMC2209 UART configuration, TILT motion, homing, or M3 work until
M2 has been physically tested and further work is explicitly requested.
