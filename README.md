# Sentry MCU Firmware

Custom motion-control firmware for the **Sentry** project, targeting the **BIGTREETECH SKR Mini E3 V2.0**.

This repository repurposes the SKR Mini E3 V2.0 from a 3D-printer control board into a dedicated two-axis embedded motion controller for a computer-vision-guided pan/tilt system. It also contains an isolated Raspberry Pi camera acquisition service for local viewing and dataset collection.

The firmware is intentionally independent of Marlin, Klipper, and other printer-specific firmware.

Its purpose is to provide a small, deterministic, hardware-specific control layer between a Raspberry Pi and the SKR's integrated stepper drivers.

---

# Project Status

This project is under active development.

The README should be updated as firmware capabilities are added and hardware behavior is validated.

Current development focus:

```text
M4: two-axis PAN/TILT motion and bounded TILT commissioning
```

- M0 hardware validation: **PASS**
- M1 hardware validation: **PASS**
- M2 hardware validation: **PASS**
- M3 hardware validation: **PASS**

M3 passed powered-board validation at 40000 baud with separated transmit and
receive phases, zero UART errors, zero retries, and repeatable independent PAN
and TILT configuration. M4 development may therefore build on the validated
driver transport and configuration.

Implemented behavior:

- application and vector table linked at `0x08007000`
- all four integrated stepper drivers disabled at startup
- all STEP and DIR outputs held low
- heater and controllable fan MOSFET outputs explicitly held OFF
- polling-based USB CDC ACM, independent of DTR/RTS state
- bounded text protocol supporting independent PAN/TILT motion and commissioning
- 20 kHz TIM4 STEP scheduling with divided 1 kHz control/state updates
- TIM2_CH3 PAN and TIM3_CH3 TILT hardware one-shot STEP pulses
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
ZAM+ZBM / Z driver        = TILT (parallel outputs, one logical axis)
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
< OK SENTRY-MCU 0.4.0 SKR_MINI_E3_V2
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

Velocity is expressed only in raw steps per second. In the hardware-validated
M2 build, zero and signed values from 1 through 1000 steps/s were accepted.
Acceleration remains fixed at 2000 steps/s². The current M4 limits are
documented with the two-axis protocol below.

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
to PAN and TILT and is allowed only while both axes are fully disabled. Configuration
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

M4 adds independent and atomic two-axis motion:

```text
ENABLE PAN
ENABLE TILT
DISABLE PAN
DISABLE TILT
VEL PAN <signed_steps_per_second>
VEL TILT <signed_steps_per_second>
VEL BOTH <pan_steps_per_second> <tilt_steps_per_second>
STOP
STATE?
STATE? PAN
STATE? TILT
ENDSTOP? TILT
HOME TILT
JOG TILT <signed_steps>
DIR-CHECK TILT HIGH|LOW
```

Bare `STATE?` remains the M2 PAN response. `VEL BOTH` validates both complete
commands before changing either target, and each nonzero target has its own
1000 ms lease. `STOP` commands both axes toward zero. TILT velocity and JOG
require a successful home; reset or `DISABLE TILT` clears the reference.
The explicit commissioning velocity limits are 10000 steps/s for PAN and 5000
steps/s for TILT. Commands beyond an axis limit are rejected, never clamped.

The Z endstop is PC2 with an internal pull-up. The installed switch path was
hardware-validated as LOW when released and HIGH when pressed, so it is
interpreted as active-high. `ENDSTOP? TILT` is read-only and must be manually
checked before homing. TILT direction is hardware-validated and compiled in:
PC5 LOW rotates CCW toward home, while PC5 HIGH rotates CW away from home.
Logical negative motion and HOME therefore use LOW; logical positive motion
uses HIGH. Raw `DIR-CHECK TILT` commands are rejected once calibrated.

Homing is asynchronous and bounded. An initially active switch is released at
3000 steps/s, then the first negative approach targets 4000 steps/s with a
20000 external-STEP search limit. After first contact, TILT moves positive at
3000 steps/s until release is stable, with a 500-step release-search bound,
then moves exactly 100 additional clearance steps before stopping and
re-approaching at 400 steps/s. The slow re-approach is bounded to 600 steps. A
raw trigger immediately suppresses further negative edges at the 20 kHz
scheduler rate; five stable 1 kHz control samples qualify trigger and release.
The second stable trigger establishes mechanical coordinate zero, then TILT
parks 800 positive steps at 2000 steps/s. Successful HOME therefore reports
`HOMED=1 POS=800` while the switch reference remains position zero.

There is intentionally no positive TILT software maximum in this M4 build.
`STATE? TILT` reports `MAX_CONFIGURED=0 MAX_LIMIT=0`. Use only repeated bounded
`JOG TILT +50` commands while observing the mechanism, record the largest safe
physical position, then select a later `TILT_MAX_STEPS` inside it with a safety
margin. Keep power disconnect immediately accessible during commissioning.

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
OK SENTRY-MCU 0.4.0 SKR_MINI_E3_V2
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

The shared TMC bus uses STM32 USART3 at 40000 baud, 8 data bits, no parity, and
one stop bit. This is the hardware-validated M3 transport selected after
powered-board diagnostics showed recovered FE, ORE, and NE events at 115200
baud. USART3 AFIO partial remap (`01`) routes TX to PC10 and
RX to PC11; full remap is not applicable because it routes to PD8/PD9. Every
AFIO write also preserves TIM2 partial-remap 2 and the established disabled-SWJ
setting, leaving M2 PAN STEP/DIR generation unchanged.

The SKR schematic combines TX and RX through R72/R73 (100 ohm) and R74
(1 kohm). The final transport separates transmit and receive phases. USART3 RX
is disabled while the four- or
eight-byte request is transmitted. After the final byte reaches `TC`, firmware
clears TX-associated RX/error state while RX remains disabled, enables RX, and
accepts only the expected eight-byte TMC read reply. Writes do not require an
echo; IFCNT advancement and masked readable-register checks remain their
authoritative verification.

Each attempt remains bounded to 5 ms and each transaction receives at most
three attempts. At 40000 baud, an 8N1 read occupies 1.0 ms for the four-byte
request, 0.6 ms for unchanged `SENDDELAY=2` (24 bit times), and 2.0 ms for the
eight-byte reply. Firmware then keeps the master transmitter idle-high for the
datasheet's four-bit TMC bus-release interval. One bit is 25 us at 40 kbaud, so
the baud-derived guard is 100 us and the complete read is about 3.7 ms, leaving
1.3 ms within the unchanged timeout. An eight-byte write occupies about 2.0
ms. Error recovery now derives a twelve-bit idle-high interval from the same
baud constant: 300 us at 40 kbaud. Baud, SENDDELAY, retry count, transaction
timeout, diagnostics, and all driver and motion safety behavior are otherwise
unchanged. All TMC I/O runs during boot or a USB command in main context;
motion timer interrupt handlers never call it.

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

ZAM and ZBM remain parallel outputs from the single TILT/Z driver and are never
treated as independent axes.

## M3 Physical Validation

Status: **PASS**.

Repeated powered-board testing verified stable PAN address 2 and TILT address
1 communication at 40000 baud with separated TX/RX phases, at least four bit
times of post-reply guard, and at least twelve bit times of error-recovery
guard. Diagnostics remained at zero ORE, NE, FE, and PE events with zero
retries. Both drivers repeatedly verified 520 mA RMS run current, 275 mA RMS
hold current, 16 microsteps, interpolation, and StealthChop. Repeated `DRIVER
CONFIGURE` was idempotent with IFCNT remaining 9. PAN 300 and 800 step/s motion,
the M2 deadman, and reset/unconfigured safety regressions all passed.

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
`DRIVER CONFIGURE` after every reset.

## M4 Timer Architecture and Commissioning

TIM4 is the only motion interrupt and runs at 20 kHz. Each axis has an
independent scheduler accumulator; every interrupt adds the absolute current
velocity, subtracts 20000 on overflow, and may start at most one hardware STEP
one-shot per axis. PAN and TILT may emit simultaneously. This supports a
20000-step/s architectural scheduler capacity while keeping the commissioning
command limits lower at PAN=10000 and TILT=5000 steps/s.

A counter runs acceleration, reversal, command leases, disable transitions,
and the TILT reference state machine only once every 20 scheduler interrupts,
preserving their validated 1 kHz semantics. Position and homing/JOG edge counts
advance only after an external STEP edge is accepted and committed. Raw TILT
endstop state is still checked on every scheduler interrupt. At 72 MHz, the
20 kHz interrupt period provides approximately 3600 CPU cycles per TIM4 ISR.

TIM2_CH3 remains partial-remapped to PAN PB10. TIM3 is no longer the control
timer: default TIM3_CH3 drives TILT PB0. Both STEP timers run at 1 us resolution
with PSC=71, ARR=3, PWM2, and CCR3=2, producing a rising edge at 2 us and falling
edge at 4 us without an ISR. PAN DIR/ENABLE remain PB2/PB11; TILT DIR/ENABLE are
PC5/PB1. ZAM and ZBM are parallel connectors on the one Z/TILT driver.

Use this staged physical workflow:

```bash
python3 host/sentry_mcu.py --device <device> configure-drivers
python3 host/sentry_mcu.py --device <device> endstop
# Confirm released is ENDSTOP=0 RAW=0 and pressed is ENDSTOP=1 RAW=1.
python3 host/sentry_mcu.py --device <device> home-tilt
python3 host/sentry_mcu.py --device <device> jog-tilt --steps 50
```

The calibrated HOME search is bounded to 20000 steps (approximately 50 mm at
400 external steps/mm) and finishes parked approximately 2 mm away at
`POS=800`. Keep immediate access to power disconnect, and do not run HOME
unless the switch state and direction have been physically verified.

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
│   ├── sentry_mcu.py
│   ├── camera/
│   │   ├── camera_service.py
│   │   ├── picamera2_backend.py
│   │   ├── storage.py
│   │   └── stream_server.py
│   └── vision/
│       ├── detector.py
│       ├── target_selector.py
│       ├── tracking_state.py
│       └── vision_service.py
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

Status: hardware-validated **PASS**.

Goals:

- UART communication
- driver detection
- current configuration
- microstepping
- StealthChop configuration
- status reporting

---

## M4: Two-Axis Motion

Status: implemented and build-tested; physical direction, endstop, homing, and
travel calibration pending.

Goals:

- pan and tilt
- simultaneous motion
- independent velocity commands
- acceleration limits
- bounded TILT direction commissioning and PC2 homing
- exact-step calibration JOG

The positive TILT software maximum remains intentionally deferred until the
physical maximum and safety margin are measured.

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

# Raspberry Pi Camera Service

`host.camera` owns one Raspberry Pi CSI camera and makes its frames available to
the local browser stream, MP4 recording, dataset capture, and the optional
`host.vision` worker. No browser client, dataset worker, or vision component
opens another camera object.

The supported deployment target is Raspberry Pi OS with Picamera2/libcamera.
Install the required system packages:

```bash
sudo apt update
sudo apt install --no-install-recommends python3-picamera2 python3-flask python3-av python3-pil
```

Picamera2 depends on Raspberry Pi OS system libraries. If a virtual environment
is required, create it with access to system packages:

```bash
python3 -m venv --system-site-packages .venv
source .venv/bin/activate
```

Camera-only operation does not require Ultralytics. To enable optional YOLO
detection in that system-packages environment, install it separately:

```bash
python -m pip install ultralytics
```

Connect the CSI camera while the Pi is powered off. After boot, verify that the
camera and libcamera pipeline work before starting Sentry:

```bash
rpicam-hello -t 5000
```

Start the service from the repository root:

```bash
python3 -m host.camera.stream_server
```

The default configuration is 1280x720 at 30 FPS, port 8080, a two-second
dataset interval, and a 1 GiB minimum-free-space threshold. Override it with:

```bash
python3 -m host.camera.stream_server \
    --host 0.0.0.0 \
    --port 8080 \
    --width 1280 \
    --height 720 \
    --fps 30 \
    --capture-interval 2 \
    --min-free-gb 1
```

Detection is disabled at startup, including when a model is configured. Copy a
trained `.pt` model to the Pi and supply its path when starting the camera:

```bash
python3 -m host.camera.stream_server \
    --model /home/pi/models/animal_mouse.pt \
    --target-class animal_mouse \
    --confidence 0.5 \
    --detection-fps 5 \
    --inference-size 640 \
    --dead-zone-x 0.05 \
    --dead-zone-y 0.05
```

The model is loaded synchronously the first time **Enable Vision** is selected,
then retained across disable/enable cycles. A missing model, absent target
class, or inference failure disables only vision; streaming, recording, and
captures continue to operate.

Find the Pi's local address with:

```bash
hostname -I
```

From another device on the same network, open:

```text
http://<PI_IP>:8080/
```

The page shows the live MJPEG feed, resolution, FPS, recording state, and
dataset-capture state. Its controls start and stop recording, save one frame,
and start or stop interval dataset capture. Dataset capture is disabled at
startup and takes one image immediately when enabled, then approximately one
per configured interval without queuing missed captures.

When vision is enabled, a transparent browser canvas draws the latest boxes,
selected target, centers, dead zone, state, and normalized errors over the raw
MJPEG image. `/stream`, saved images, and MP4 files remain unannotated. The
overlay polls status about four times per second and can therefore trail the
live image by an inference interval.

Recordings use timestamped MP4 filenames under `data/recordings/`. Starting or
stopping recording does not restart the camera, repeated start/stop requests
are safe, and existing files are never overwritten. A recording is refused if
free space is below the configured threshold.

Images are stored under `data/captures/raw/`, with metadata appended to
`data/captures/capture_metadata.csv`. While no recording is active, a capture
briefly pauses MJPEG, switches to the camera's largest still mode, captures,
then restores video streaming. While recording, captures use the uninterrupted
configured video frame so the MP4 is not split. Metadata records the actual
dimensions and source mode for both cases.

Manual captures are refused below the disk threshold. Interval collection
disables itself and logs a warning when storage is low. The service never
deletes recordings or captures automatically.

The server is intentionally unauthenticated and unencrypted for local-network
development. Do not expose port 8080 directly to the public internet. Stop the
service with Ctrl+C or SIGTERM; active MP4 output is finalized before the
camera and background worker are closed.

## Camera CLI and HTTP API

The CLI also supports `--data-dir` and `--log-level`. Run
`python3 -m host.camera.stream_server --help` for the complete argument list.

The local API is:

```text
GET  /
GET  /stream
GET  /api/status
POST /api/record/start
POST /api/record/stop
POST /api/capture
POST /api/dataset/start
POST /api/dataset/stop
POST /api/vision/start
POST /api/vision/stop
```

Vision status is included in `GET /api/status` and every mutation response.
`SEARCHING` has no target, `DETECTED` is the first result after acquisition,
and later target frames are `TRACKING` or `LOCKED`. Pan error is negative to
the left and positive to the right; tilt error is negative above center and
positive below. Dead-zone comparisons are inclusive and do not zero the raw
normalized errors.

The worker processes only the latest requested camera frame and never queues
inference work. `--detection-fps` is a maximum rather than a guarantee. The
reported inference time and completed detection rate help determine whether a
smaller input or later model export is warranted. The
[Ultralytics prediction documentation](https://docs.ultralytics.com/modes/predict)
describes the model arguments and result boxes used here. `.pt` is the initial
supported path; the
[Raspberry Pi guide](https://docs.ultralytics.com/guides/raspberry-pi) identifies
NCNN as a faster ARM deployment option for later optimization.

Generic camera logic can be tested without Raspberry Pi hardware:

```bash
make camera-test
make vision-test
```

---

# Current Scope Boundary

This repository contains the embedded motion-control subsystem, the isolated
Raspberry Pi camera acquisition service, and the optional `host/vision/` layer
documented above.

The vision exception is limited to optional YOLO detection, exact-class
highest-confidence selection, camera-coordinate error/state reporting, and a
browser overlay. ByteTrack, identity persistence, annotated stored media,
vision-driven motor commands, autonomous behavior, and the full Sentry
application UI remain out of scope.

The firmware exposes a clean motion-control interface without depending on the
camera or vision services. Neither host subsystem sends motor commands or
changes the MCU protocol.

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
