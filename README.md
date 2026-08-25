# Sentry MCU Firmware

Custom motion-control firmware for the **Sentry** project, targeting the **BIGTREETECH SKR Mini E3 V2.0**.

This repository repurposes the SKR Mini E3 V2.0 from a 3D-printer control board into a dedicated two-axis embedded motion controller for a computer-vision-guided pan/tilt system.

The firmware is intentionally independent of Marlin, Klipper, and other printer-specific firmware.

Its purpose is to provide a small, deterministic, hardware-specific control layer between a Raspberry Pi and the SKR's integrated stepper drivers.

---

# Project Status

This project is under active development.

The README should be updated as firmware capabilities are added and hardware behavior is validated.

Current development focus:

```text
M3: TMC2209 UART hardware validation
```

- M0 hardware validation: **PASS**
- M1 hardware validation: **PASS**
- M2 hardware validation: **PASS**

M3 is implemented and build-verified. Its USART3 electrical behavior and TMC
configuration remain hardware-unvalidated until the M3-A and M3-B procedures
below pass on the SKR Mini E3 V2.0.

Implemented behavior:

- application and vector table linked at `0x08007000`
- all four integrated stepper drivers disabled at startup
- all STEP and DIR outputs held low
- heater and controllable fan MOSFET outputs explicitly held OFF
- polling-based USB CDC ACM, independent of DTR/RTS state
- bounded text protocol supporting `PING`, `INFO`, and M2 PAN commands
- TIM3-driven integer acceleration and motion-command deadman
- TIM2_CH3 hardware one-shot STEP pulses on PB10
- polling USART3 TMC2209 diagnostics and fixed, host-triggered configuration
- PAN enable gating on a configured, nonfatal driver state
- fixed-size static buffers with no dynamic allocation

Initial target:

```text
Raspberry Pi
    |
    | USB
    v
SKR Mini E3 V2.0
    |
    v
custom Sentry firmware
```

The first hardware milestone is:

```text
Pi sends:     PING
SKR returns:  OK PONG
```

No motor movement should occur until basic board initialization and USB communication have been verified.

---

# System Overview

Sentry separates high-level perception from low-level real-time motion control.

```text
Camera
   |
   v
Raspberry Pi
   |
   | object detection
   | tracking
   | target selection
   | high-level motion commands
   |
   | USB
   v
SKR Mini E3 V2.0
   |
   | real-time motion control
   | step generation
   | acceleration limiting
   | driver management
   | fault handling
   |
   v
TMC2209 Stepper Drivers
   |
   +------> Pan Motor
   |
   +------> Tilt Motor
```

The Raspberry Pi decides **where the system should move**.

The STM32 decides **how the motors physically execute that motion**.

The Pi must not generate individual step pulses.

---

# Target Hardware

Primary development target:

- BIGTREETECH SKR Mini E3 V2.0
- STM32F103RC microcontroller
- Integrated TMC2209 stepper drivers
- Raspberry Pi host
- USB connection between Pi and SKR
- Two stepper motors

Sentry motor mapping:

```text
YM connector / Y driver  = PAN
ZAM connector / Z driver = TILT (not implemented yet)
```

ZAM and ZBM are parallel motor connectors driven by the same Z driver. They
must never be modeled as independent axes.

The SKR's remaining printer-specific hardware is not currently used.

---

# Why Custom Firmware?

The SKR Mini E3 V2.0 contains most of the hardware needed for a compact motion-control subsystem:

- STM32 microcontroller
- four integrated TMC2209 drivers
- stepper connectors
- limit-switch inputs
- UART-controlled motor drivers
- USB
- microSD
- regulated power electronics

Traditional printer firmware contains many systems that Sentry does not require:

- extrusion
- hotend control
- heated-bed control
- slicer compatibility
- printer coordinate conventions
- temperature management
- print queues
- printer-specific G-code behavior

Sentry instead needs a small real-time motion controller optimized for continuously changing pan/tilt commands.

---

# Firmware Responsibilities

The STM32 firmware is responsible for:

- USB communication
- command parsing
- deterministic step generation
- motor enable/disable
- direction control
- acceleration and deceleration
- local position tracking
- TMC2209 configuration
- motor-current control
- Hall sensor or endstop inputs
- axis limits
- communication watchdog
- fault detection
- emergency stop behavior
- telemetry

The firmware does **not** perform:

- object detection
- camera processing
- YOLO inference
- target selection
- tracking prediction
- user-interface rendering
- autonomous behavioral decisions

Those functions belong on the Raspberry Pi.

---

# Control Philosophy

The Sentry motion controller is intended to behave more like a compact servo-motion subsystem than a printer controller.

A typical runtime flow will be:

```text
camera frame
    |
    v
target position
    |
    v
tracking controller
    |
    v
desired angular velocity
    |
    | USB
    v
STM32
    |
    v
acceleration limiter
    |
    v
step generator
    |
    v
TMC2209
```

The Pi may update motion commands many times per second.

The STM32 continues generating precise motor timing independently between host updates.

---

# Initial Communication Protocol

Development begins with a simple human-readable USB CDC protocol.

Example:

```text
> PING
< OK PONG
```

Firmware information:

```text
> INFO
< OK SENTRY-MCU 0.3.0 SKR_MINI_E3_V2
```

Commands are ASCII, case-sensitive, and terminated by CR, LF, or CRLF.
Leading and trailing spaces or tabs are ignored. Firmware responses use CRLF.

Additional M0/M1 errors are:

```text
ERR EMPTY_COMMAND
ERR UNKNOWN_COMMAND
ERR COMMAND_TOO_LONG
```

The command buffer accepts at most 63 bytes before the line terminator. An
oversized line is discarded through its terminator and produces one bounded
error response.

USB descriptors currently use:

```text
VID:          0x1209
PID:          0x0001
Manufacturer: Sentry
Product:      Sentry Motion Controller
Serial:       0001
```

`1209:0001` is a shared, non-unique test identifier intended only for
development. Obtain a project-specific VID/PID allocation before distributing
hardware or relying on VID/PID uniqueness.

M2 commands are:

```text
ENABLE PAN
DISABLE PAN
VEL PAN <signed_steps_per_second>
STOP
STATE?
```

`ENABLE PAN` is required before any velocity command. A moving
`DISABLE PAN` replies `OK PAN DISABLING`, performs a controlled ramp to zero,
and disables the Y driver only after motion stops. An already stopped disable
replies `OK PAN DISABLED`.

Velocity is expressed only in raw steps per second. Zero and signed values
from 1 through 1000 steps/s are accepted. The initial acceleration is fixed at
2000 steps/s². These conservative M2 values validate firmware timing; they do
not represent final mechanical calibration or performance.

`STATE?` has the bounded, machine-readable form:

```text
OK PAN ENABLED=1 DISABLING=0 MOVING=1 POS=123 VEL=180 TARGET=200 TIMEOUT=0
```

`POS` is an unreferenced signed step count starting at zero after boot. It is
not a home position or physical angle. `TIMEOUT` latches when the M2 motion
lease expires and is cleared only by the next accepted nonzero velocity.

Additional M2 errors are:

```text
ERR BAD_ARGUMENT
ERR VELOCITY_RANGE
ERR PAN_DISABLED
ERR PAN_DISABLING
```

M3 adds:

```text
DRIVER? PAN
DRIVER? TILT
DRIVER-DIAG? PAN
DRIVER-DIAG? TILT
DRIVER CONFIGURE
```

Boot probes both TMC2209 addresses and verifies the non-behavior-changing
write path, but intentionally does not configure current, microsteps, or the
chopper. `DRIVER CONFIGURE` applies the fixed M3 configuration independently
to PAN and TILT and is allowed only while PAN is fully disabled. Configuration
lasts for the current power cycle; it writes neither OTP nor MCU flash.
Drivers already verified as configured and nonfatal are left untouched, so a
retry can recover one driver without rewriting the other or incrementing its
IFCNT.

Communication health and configuration validity are tracked separately. A
transient UART error temporarily blocks enable, but a later successful status
refresh restores `CONFIGURED` when the verified settings remain valid. The
latched `GSTAT.reset` indication is cleared after configuration; if it is seen
again, the cached configuration is invalidated, its commanded-setting metadata
is cleared, and `DRIVER CONFIGURE` is required again. Reset is not classified
as a fatal driver fault.

`ENABLE PAN` returns `ERR PAN_DRIVER_NOT_READY` until PAN is configured and
has no detected fatal status. Driver queries never configure or move a motor.
A successful configuration returns:

```text
OK DRIVERS PAN=CONFIGURED TILT=CONFIGURED
```

A partial or total failure returns each retained driver state:

```text
ERR DRIVER_CONFIG PAN=<state> TILT=<state>
```

Driver status is a bounded machine-readable line containing address, presence,
configuration state, last error, raw IOIN/GSTAT/DRV_STATUS, commanded current
and microstep settings, decoded overtemperature warning, standstill,
StealthChop activity, and fatal state. For example:

```text
OK DRIVER PAN ADDR=2 PRESENT=1 CONFIGURED=1 ERR=NONE IFCNT=7 IOIN=0x21000000 GSTAT=0x00000000 DRV=0xC0100000 RUN_MA=520 HOLD_MA=275 MSTEP=16 INTPOL=1 MODE=STEALTHCHOP OTPW=0 STST=1 STEALTH=1 FATAL=0
```

`DRIVER-DIAG?` is a read-only snapshot and performs no TMC transaction. It
reports boot-lifetime, saturating USART3 ORE/NE/FE/PE counters and the last
address/register active for a raw USART error. Those counters are global
because PAN and TILT share USART3. Retry totals, the last failed transaction,
transport-versus-parser error, captured UART flags, and the last failed
probe/configuration stage are retained independently for each driver. A later
successful query does not erase this history. For example:

```text
OK DRIVER_DIAG TILT ADDR=1 UART_ORE=0 UART_NE=4 UART_FE=0 UART_PE=0 UART_LAST_ADDR=1 UART_LAST_OP=READ UART_LAST_REG=0x06 UART_FLAGS=0x04 UART_RETRIES=8 LAST_OP=READ LAST_REG=0x06 LAST_ATTEMPT=2 TRANSPORT=UART PARSER=NONE LAST_CFG_STAGE=PROBE_IOIN LAST_CFG_REG=0x06 LAST_CFG_PHASE=READ LAST_CFG_ERR=UART
```

`UART_FLAGS` uses the STM32 USART status bit positions in a compact mask:
PE=`0x01`, FE=`0x02`, NE=`0x04`, and ORE=`0x08`. Counters and retained history
reset only when the MCU resets; no host command clears them.

The protocol may evolve as requirements become clearer.

A binary protocol is not currently required.

---

# Bootloader

The factory BIGTREETECH SD-card bootloader is intentionally preserved.

The SKR Mini E3 V2.0 uses a 28 KiB bootloader.

The Sentry application therefore begins at:

```text
0x08007000
```

Flash layout:

```text
0x08000000
+--------------------------+
| BTT bootloader           |
| 28 KiB                   |
+--------------------------+
0x08007000
| Sentry firmware          |
|                          |
+--------------------------+
```

Do not overwrite the original BTT bootloader.

---

# Firmware Flashing

The current flashing method uses the existing SKR microSD bootloader.

Build the project:

```bash
make
```

Requirements:

- GNU Make
- Git
- Python 3 (the pinned libopencm3 generator is invoked explicitly so builds
  also work from CRLF checkouts)
- Arm GNU Toolchain (`arm-none-eabi-gcc`, `arm-none-eabi-objcopy`, and
  `arm-none-eabi-size` on `PATH`)

The first `make` automatically fetches libopencm3 v0.8.0 at the pinned commit
`d60d7802fd20821a766675545b6ef7a2207207a3` into `lib/libopencm3`. A checkout at
any other revision fails the build rather than silently using different code.

Build outputs:

```text
build/sentry.elf
build/sentry.bin
build/sentry.map
firmware.bin
```

Run the host-compiled command-framing tests separately with:

```bash
make test
```

Expected binary:

```text
firmware.bin
```

Flash procedure:

1. Format a microSD card as FAT32.
2. Copy `firmware.bin` to the root of the card.
3. Power off the SKR.
4. Insert the microSD card.
5. Power on the SKR.
6. Allow the onboard bootloader to update the STM32 application.

Keep immediate access to the power disconnect during first bring-up. Before
connecting motors or loads, verify with a meter that heater and fan controls
remain off and that all motor-enable pins remain inactive.

## Raspberry Pi USB Test

After flashing and reconnecting USB, locate the CDC ACM device:

```bash
dmesg --follow
ls -l /dev/serial/by-id/ /dev/ttyACM*
```

Select the discovered device (adjust `/dev/ttyACM0` if needed), configure the
terminal without asserting any required modem-control state, and send `PING`:

```bash
DEVICE=/dev/ttyACM0
stty -F "$DEVICE" 115200 raw -echo
timeout 2 cat "$DEVICE" &
READER_PID=$!
sleep 0.1
printf 'PING\r\n' > "$DEVICE"
wait "$READER_PID"
```

Expected response:

```text
OK PONG
```

Repeat with `printf 'INFO\r\n' > "$DEVICE"` and expect:

```text
OK SENTRY-MCU 0.3.0 SKR_MINI_E3_V2
```

These commands are the physical validation procedure used for M1. Hardware
validation has been reported complete by the operator.

## M2 Timer Architecture

PAN motion uses two STM32 hardware timers after the system clock reaches
72 MHz:

- TIM3 is clocked at 72 MHz from APB1, divided by 72, and counts 1000 ticks per
  update. Its 1 kHz interrupt runs the acceleration limiter, the 1000 ms
  motion lease, and an integer phase accumulator.
- TIM2 is clocked at 72 MHz from APB1 and divided by 72 for 1 us resolution.
  TIM2 partial-remap 2 routes channel 3 to PB10. Each phase overflow starts a
  hardware one-shot whose output rises after 2 us and falls 2 us later,
  producing a 2 us active-high STEP pulse before returning to a low idle
  state. TIM2_CH4 is not enabled, so remapped PB11 remains the PAN driver's
  ordinary active-low enable GPIO.

TIM3 can schedule at most one pulse per control tick, so the supported M2
range is 1 to 1000 steps/s. Rates that do not divide 1000 have deterministic
1 ms interval quantization while retaining the requested average rate. USB
handling remains in the main loop and neither controls nor delays STEP timing.

Positive PAN is defined logically as DIR low and negative PAN as DIR high.
This does not yet claim a physical clockwise or counterclockwise convention.
Reversal commands ramp to an explicit zero-velocity tick, discard the old
fractional phase, change DIR while STEP is low, and then ramp the other way.

Only a valid nonzero `VEL PAN ...` command starts or refreshes the 1000 ms
motion lease. Expiry changes the target to zero and uses controlled
deceleration; it does not hard-disable the driver. `PING`, `INFO`, and `STATE?`
do not refresh the lease.

## Raspberry Pi M2 Validation Utility

Install the host dependency:

```bash
python3 -m pip install pyserial
```

The utility can discover the development `1209:0001` USB identity or accept an
explicit CDC path. Non-motion checks include:

```bash
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> ping
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> info
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> state
```

Importing the module does nothing. Movement requires the explicit
`motor-test` action.

## First PAN Motion Test

Prepare the hardware before powering it:

1. Connect the PAN motor only to YM; disconnect X, ZAM, ZBM, and E motors.
2. Disconnect heater and fan loads where practical.
3. Arrange immediate access to the main power disconnect.
4. Power the SKR normally and connect USB to the Raspberry Pi.
5. Confirm the board enumerates and that `PING`, `INFO`, and `STATE?` work.

Run the conservative default test, which requests 150 steps/s for 0.5 seconds
in each direction and refreshes the motion lease every 200 ms:

```bash
python3 host/sentry_mcu.py \
  --device /dev/serial/by-id/<sentry-device> \
  motor-test --velocity 150 --duration 0.5
```

The utility first queries PAN. It skips configuration when PAN is already
authoritatively configured and fault-free; otherwise it attempts configuration
and re-queries PAN before enable. A TILT-only configuration failure is reported
as a warning and does not prevent the PAN test when PAN's refreshed status is
ready. The firmware enable gate remains authoritative. The utility then enables
PAN, tests positive motion, stops and waits for zero, tests negative motion,
stops again, disables PAN, and prints final state. Stop the test and disconnect
power immediately if motion or any unused output is unexpected.

The following M2 acceptance checks passed on physical hardware using firmware
0.2.1 with PAN connected to YM/Y:

1. The board still boots safely.
2. USB still enumerates reliably.
3. `PING` and `INFO` still work.
4. PAN remains motionless and disabled after boot.
5. `ENABLE PAN` enables only the Y-driver PAN output through YM.
6. Positive velocity produces controlled rotation.
7. Negative velocity produces controlled rotation in the opposite direction.
8. STEP frequency approximately matches the requested steps/s, and the pulse
   high time is approximately 2 us.
9. Velocity ramps instead of jumping immediately.
10. `STOP` produces controlled deceleration.
11. The position counter changes with emitted steps.
12. `DISABLE PAN` leaves the driver disabled and STEP low.
13. Motion-command timeout stops motion if host refreshes cease.
14. USB remains responsive during motion.
15. X, Z, E, heater, and fan outputs remain inactive.
16. Repeated power cycles still boot safely.

The operator additionally recorded smooth positive and negative motion during
repeated 800 steps/s, 1.0 s tests. The first direction ended at `POS=800` and
the reverse returned exactly to `POS=0`. Acceleration and controlled
deceleration were visibly smooth; STOP, ENABLE, DISABLE, and USB responsiveness
all passed. The 1000 ms deadman was tested with one `VEL PAN 300` followed by
host silence and reported `POS=299 VEL=0 TARGET=0 TIMEOUT=1`. No unintended
motion was observed on other outputs.

## M3 TMC2209 Architecture

The shared TMC bus currently uses STM32 USART3 at 40000 baud, 8 data bits, no
parity, and one stop bit. This is an M3 transport experiment based on the
known-good Klipper TMC UART rate after powered-board diagnostics showed
recovered FE, ORE, and NE events at 115200 baud; it is not yet a final
architecture decision. USART3 AFIO partial remap (`01`) routes TX to PC10 and
RX to PC11; full remap is not applicable because it routes to PD8/PD9. Every
AFIO write also preserves TIM2 partial-remap 2 and the established disabled-SWJ
setting, leaving M2 PAN STEP/DIR generation unchanged.

The SKR schematic combines TX and RX through R72/R73 (100 ohm) and R74
(1 kohm), so the polling transport expects transmitted bytes to appear on RX.
It captures at most one exact request echo plus one reply, discards only an
exact echo, validates the reply, bounds each transaction attempt to 5 ms, and
retries at most three times. At 40000 baud, an 8N1 read occupies 1.0 ms for the
four-byte request, 0.6 ms for unchanged `SENDDELAY=2` (24 bit times), and
2.0 ms for the eight-byte reply: 3.6 ms total, leaving 1.4 ms within the
unchanged timeout. An eight-byte write plus the existing 100 us capture window
takes about 2.1 ms. Recovery remains unchanged at 200 us, which is 8 bit times
at this experimental rate; no recovery timing, retry, echo, or RX/TX sequencing
change is included in this experiment. All TMC I/O runs during boot or a USB
command in main context; TIM2/TIM3 interrupt handlers never call it.

Datagrams follow TMC2209 Rev. 1.09 directly: sync `0x05`, four-byte reads,
eight-byte writes/replies, data most-significant byte first, master reply
address `0xFF`, and CRC-8/ATM polynomial `0x07`, initial zero, with each input
byte processed least-significant bit first. PAN/Y is address 2 and TILT/Z is
address 1. IOIN must report version `0x21`. `NODECONF.SENDDELAY=2` supports the
shared multi-node bus. Writes are accepted only after IFCNT advances by one
modulo 256; readable configuration registers also receive masked readback.

The verified [BTT V2.0 schematic](https://github.com/bigtreetech/BIGTREETECH-SKR-mini-E3/blob/master/hardware/BTT%20SKR%20MINI%20E3%20V2.0/Hardware/BTT%20SKR%20MINI%20E3%20V2.0SCHpdf.PDF)
labels each external phase sense resistor `0R11`, or 0.11 ohm. M3 selects
digital current scaling and external sense
resistors (`I_scale_analog=0`, `internal_Rsense=0`) with `VSENSE=1`. Using the
datasheet relationship with the 20 milliohm internal contribution gives
IRUN=16 as 520 mA RMS and IHOLD=8 as 275 mA RMS (52.9% of run current).
`IHOLDDELAY=8` and `TPOWERDOWN=20`. The calculator rejects requests over the
firmware's 580 mA safety ceiling, and no arbitrary-current host command exists.

The fixed configuration selects register-controlled 16 microsteps (`MRES=4`)
and interpolation to 256. One firmware position step remains one emitted STEP
rising edge. StealthChop2 remains selected at every speed (`TPWMTHRS=0`) with
automatic scaling and gradient, PWM frequency 1, offset 36, gradient 14,
regulator 8, and limit 12. CHOPCONF uses TOFF 5, TBL 2, HSTRT 4, HEND 0,
single-edge stepping, and enabled short protections. VACTUAL is never written.
IHOLD_IRUN is write-only on the TMC2209, so its commanded current is verified
through IFCNT rather than literal readback; readable registers use masked
readback and DRV_STATUS retains the live `CS_ACTUAL` bits in its raw value.

TILT is probed and configured to prove independent addressability, but it has
no STEP generation or enable command. PB1 remains inactive high, and ZAM/ZBM
remain parallel outputs from one disabled Z driver.

## M3 Physical Validation

M3-A is communication-only. Motors may remain disconnected, but normal SKR
main power must be present:

```bash
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> ping
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> info
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> driver pan
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> driver tilt
```

Each host `driver` action performs the live `DRIVER?` query first and then
prints the cached `DRIVER-DIAG?` snapshot. Repeating the command therefore
provides a bounded non-motion UART reliability test without configuring or
enabling either driver.

Confirm PAN only at address 2, TILT only at address 1, CRC-valid reads and
IFCNT write verification, both enables inactive, and no activity on unused
outputs.

M3-B explicitly configures both drivers for this power cycle and repeats the
proven PAN motion test:

```bash
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> configure-drivers
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> driver pan
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> driver tilt
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> motor-test --velocity 300 --duration 1.0
python3 host/sentry_mcu.py --device /dev/serial/by-id/<sentry-device> motor-test --velocity 800 --duration 1.0
```

Confirm healthy status, smooth forward/reverse motion, return to zero, no
unexpected noise or heating, STOP/deadman/USB behavior, and no unused output
activity. Because configuration is deliberately not persistent, repeat
`DRIVER CONFIGURE` after every reset. M3 remains hardware-unvalidated until
both stages and repeated-power-cycle initialization pass.

Future USB-based firmware updates may be investigated later.

The SD-card method remains the recovery mechanism.

---

# Software Stack

Current preferred implementation:

- C
- ARM GCC toolchain
- libopencm3
- STM32 hardware timers
- USB CDC
- fixed-size buffers
- static memory allocation

The firmware should remain lightweight.

Current design intentionally avoids:

- Arduino
- Marlin
- Klipper
- RTOS dependencies
- dynamic memory allocation
- printer-specific frameworks

These decisions may be revisited only if hardware testing demonstrates a clear need.

---

# Repository Structure

Planned structure:

```text
sentry-mcu/
├── README.md
├── AGENTS.md
├── Makefile
│
├── linker/
│   └── skr_mini_e3_v2.ld
│
├── include/
│   ├── board.h
│   ├── protocol.h
│   ├── motion.h
│   ├── stepper.h
│   ├── tmc2209.h
│   └── fault.h
│
├── src/
│   ├── main.c
│   ├── board.c
│   ├── usb.c
│   ├── protocol.c
│   ├── motion.c
│   ├── stepper.c
│   ├── tmc2209.c
│   └── fault.c
│
├── host/
│   └── sentry_mcu.py
│
└── lib/
    └── libopencm3/
```

This structure may evolve as firmware responsibilities become clearer.

---

# Development Roadmap

## M0: Board Bring-Up

Status: hardware-validated and complete.

Goals:

- STM32 startup
- linker configuration
- vector-table relocation
- clock initialization
- safe GPIO initialization
- motors disabled
- unused MOSFET outputs forced OFF

Success condition:

```text
Custom firmware boots repeatedly without activating hardware unexpectedly.
```

---

## M1: USB Communication

Status: hardware-validated and complete.

Goals:

- USB CDC enumeration
- bounded RX buffer
- command parser
- `PING`
- `INFO`

Success condition:

```text
PING -> OK PONG
```

over USB from the Raspberry Pi.

---

## M2: Single-Axis Motion

Status: hardware-validated and complete (PASS).

Goals:

- use the Y driver through YM as PAN
- STEP generation
- DIR control
- ENABLE control
- velocity commands
- controlled stop

Success condition:

```text
Pan motor can be commanded safely in both directions.
```

---

## M3: TMC2209 Control

Status: implemented and build-verified; M3-A/M3-B physical validation pending.

Goals:

- UART communication
- driver detection
- current configuration
- microstepping
- StealthChop configuration
- status reporting

---

## M4: Two-Axis Motion

Goals:

- pan and tilt
- simultaneous motion
- independent velocity commands
- acceleration limits

---

## M5: Sensors and Safety

Goals:

- Hall sensors or endstops
- software limits
- communication watchdog
- fault handling
- telemetry
- emergency stop behavior

---

## M6: Host Integration

Goals:

- minimal Python client
- automatic USB-device discovery
- status queries
- motion commands
- integration interface for the main Sentry application

The host utility in this repository should remain focused on firmware testing and integration.

---

# Safety

This board contains high-current outputs originally intended for heaters and fans.

Sentry does not use these outputs.

The firmware must explicitly configure unused power outputs to a safe OFF state during startup.

Motors must also default to disabled.

No motor should move automatically during boot.

A loss of host communication must not result in indefinite motion.

The firmware should eventually implement:

```text
host communication lost
        |
        v
commanded velocity -> 0
        |
        v
controlled stop
```

Hardware testing should initially use:

- low motor current
- low speed
- low acceleration
- short commands
- immediate access to power disconnect

---

# Development Principles

The firmware should remain:

```text
small
deterministic
auditable
hardware-specific
recoverable
```

Prefer explicit state machines over hidden behavior.

Prefer integer or fixed-point math in timing-critical paths.

Prefer hardware timers over software delays.

Prefer bounded buffers and static allocation.

Do not optimize communication protocols before measurements show a real need.

---

# Current Scope Boundary

This repository contains only the embedded motion-control portion of Sentry.

Related systems such as:

- computer vision
- object detection
- tracking
- camera control
- web interfaces
- user controls
- high-level behavior

belong elsewhere in the Sentry project.

The firmware should expose a clean motion-control interface to those systems without depending on them.

---

# Documentation

This README is a living project description.

Update it when:

- hardware assumptions are confirmed or changed
- milestones are completed
- supported commands change
- build or flashing procedures change
- new safety behavior is introduced
- firmware architecture changes materially

Detailed development constraints and instructions for coding agents are maintained in:

```text
AGENTS.md
```
