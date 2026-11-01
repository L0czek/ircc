# hostctl User Manual

`hostctl` is the terminal UI for the resonant converter controller. It talks to the
controller over a serial link (the control UART or the USB CDC port) using the frame
protocol described in [host_protocol.md](host_protocol.md), and can optionally receive
oscilloscope data on a second serial port (the aux data link).

## Contents

1. [Building](#building)
2. [Command-line options](#command-line-options)
3. [Screen layout](#screen-layout)
4. [Keys](#keys)
5. [Commands](#commands)
6. [Oscilloscope workflow](#oscilloscope-workflow)
7. [Aux data link](#aux-data-link)
8. [Configuration limits](#configuration-limits)
9. [Hardware emulator](#hardware-emulator)
10. [Tests](#tests)
11. [Troubleshooting](#troubleshooting)

## Building

The host tools are built as part of the top-level build (`build/host`):

```bash
cd build/host
make hostctl test_commands test_frame test_scope_core
```

`USE_EMULATOR=1` (the default) compiles the in-process hardware emulator into `hostctl`.

## Command-line options

| Option | Description | Default |
|---|---|---|
| `-p, --port <dev>` | Control serial port (`/dev/ttyACM0` for USB CDC, `/dev/ttyUSB0` for the control UART) | — |
| `-b, --baud <rate>` | Control port baud rate (ignored by USB CDC) | 115200 |
| `-d, --databits`, `-s, --stopbits`, `--parity` | Serial framing | 8, 1, none |
| `--aux-port <dev>` | Aux data link port; scope reads then default to `link=aux` | — |
| `--aux-baud <rate>` | Initial aux port baud rate (must match the device) | 115200 |
| `--poll <s>` | Request system status every *s* seconds | off |
| `--device-log <level>` | Firmware log messages to show: trace, debug, info, warn, error, off. Subscribing replays the last 16 messages, including boot errors | info |
| `-k, --keepalive <s>` | Send a ping every *s* seconds | off |
| `-e, --emulator` | Use the built-in hardware emulator instead of a serial port | off |
| `-l, --log-level` | trace, debug, info, warning, error, fatal | info |

Examples:

```bash
./hostctl -p /dev/ttyACM0 --poll 1                                # USB, status every second
./hostctl -p /dev/ttyUSB0 --aux-port /dev/ttyUSB1 --aux-baud 921600
./hostctl -e --poll 1                                             # emulator
```

## Screen layout

```
┌ Charger ───────────────┐┌ Output Regulator ──┐┌ Scope / Link ──────┐
│ [CHG ON]               ││ [OUT ON] OCP       ││ State: Done        │
│ State: Fast (CC)       ││ Mode: Buck PWM     ││ 2 ch @ 500000 Hz   │
│ Input: VAC2 (dock)     ││ Set   12.00V       ││ Samples -300..2700 │
│ VBUS: HV adapter       ││ Ilim   3.00A       ││ Trigger: software  │
│ VBUS 19.80V IBUS 1.20A ││ Rsns 0.010R        ││ Read 42% (resend 1)│
│ VBAT 12.30V IBAT 1.80A │└────────────────────┘│ Aux: open 921600 Bd│
│ VSYS 12.40V            │                      └────────────────────┘
│ VAC1  0.00V VAC2 19.80V│
│ Lim chg 12.60V 2.00A   │
│ Lim in  4.40V 3.00A    │
└────────────────────────┘
Type to enter a command ('help') | q quits | Ping: 3s ago | Status: 0s ago ●
```

- **Charger** (BQ25792): enable and fault flags, charge state, selected input, VBUS
  adapter type, ADC measurements (VBUS, IBUS, VBAT, IBAT, VSYS and both input voltages:
  VAC1 = USB-C, VAC2 = dock DC), and the configured limits.
  Below these, conditional lines decode the raw charger flags:
  - `Fault:` (red): VAC1/VAC2/VBUS/VBAT/VSYS OVP, CONV/IBUS/IBAT OCP, TSHUT, VSYS short, OTG faults;
  - `Limit:` (yellow): the regulation loop limiting the charge, one of IINDPM (input
    current), VINDPM (input voltage), TREG (thermal) or VSYSMIN;
  - `Warn:` (yellow): watchdog expired, poor source, safety timers expired, TS hot, warm,
    cool or cold.
- **Output Regulator** (TPS55289): enable, latched SCP/OCP/OVP faults, operating and
  feedback mode, and the programmed voltage, current limit and sense resistor. The
  TPS55289 has no ADC, so these are setpoints, not measurements.
- **Scope / Link**: capture state and parameters from the last `scope status`, read
  progress, aux link state, and frame errors on the control link.
- Below the panels: the voltage graph (`g` toggles it), the log (newest first) and the
  command line.

## Keys

| Key | Action |
|---|---|
| any character | Start typing a command (except `q` and `g` in normal mode) |
| `Enter` | Run the command |
| `Esc` | Cancel the command; in normal mode, quit |
| `q` | Quit (normal mode) |
| `g` | Toggle the voltage graph (normal mode) |
| `↑` / `↓` | Move focus; scroll the log when it has focus |
| `PgUp` / `PgDn` / `Home` / `End` | Scroll the log |

## Commands

Values use `key=value`; numbers may be hex (`0x3`) where integers are expected.

| Command | Description |
|---|---|
| `help [scope]` | List commands / oscilloscope help |
| `ping` | Ping the controller |
| `status` | Request system status (charger, regulator, configs) |
| `poll <s>` / `poll off` | Periodic status requests |
| `info` | Device info: board, firmware version, uptime, reset cause, FreeRTOS heap, per-thread stack headroom (flags `<-- LOW` below 128 B), PSRAM state |
| `log [trace\|debug\|info\|warn\|error]` / `log off` | Firmware log messages in the log pane (`dev <seconds> <LEVEL> text`) |
| `charger on` / `charger off` | Charging enable |
| `charger set [vchg=V] [ichg=A] [vin=V] [iin=A] [src=usb\|ac1\|ac2\|auto]` | Charge voltage and current limits, input voltage (VINDPM) and current (IINDPM) limits, input source |
| `out on` / `out off` | Output regulator enable |
| `out set [v=V] [ilim=A] [rsense=Ω]` | Output voltage, current limit and sense resistor; omitted fields keep their value |
| `scope config ...` | Configure a capture, see below |
| `scope arm` / `trigger` / `stop` / `status` | Arm, force a trigger, stop, query state |
| `scope read [file=] [ch=] [start=] [count=] [link=origin\|aux]` | Download a capture to CSV |
| `scope cancel` | Abort a running read |
| `link baud <rate>` | Set the aux data link baud rate (device and host port) |
| `link status` | Frame statistics of the control and aux links |
| `list` | List applets |
| `start <applet> [args]` / `stop <applet>` | Start / stop an applet |
| `<applet> [args]` | Send a command to a running applet |

Every set command answers with the full system status, so the panels update at once.
Failures are logged with the controller error, e.g.
`Controller: invalid parameters - BQ25792 invalid parameter (15)`.

## Oscilloscope workflow

The oscilloscope samples up to four ADC inputs (injected conversions), streams them into
PSRAM and keeps a pre-trigger history. A capture is read back afterwards.

```
scope config ch=1:1 ch=2:3:4 rate=500000 pre=1000 post=9000 trig=sw
scope arm
scope trigger            # or wait for the AWD / comparator / external trigger
scope status             # State: Done, samples -1000..9000
scope read file=run1.csv
```

`scope config` options:

| Option | Meaning |
|---|---|
| `ch=<adc>:<input>[:<smp>]` | One channel per option, in order (channel 0, 1, …), at most 4. `adc` is 1..5, `input` the ADC input number, `smp` the sampling time index 0..7 = 2.5, 6.5, 12.5, 24.5, 47.5, 92.5, 247.5, 640.5 ADC cycles |
| `rate=<Hz>` | Sample rate per channel |
| `pre=<n>` / `post=<n>` | Samples kept before and recorded after the trigger |
| `trig=sw,awd,comp<N>,ext` | Accepted trigger sources (any combination); without `trig` only software triggers are accepted |
| `awd=<ch>:<low>:<high>` | Analog watchdog trigger on channel index `ch` outside [low, high] (ADC codes) |
| `fullpre=0\|1` | Ignore triggers until the pre-trigger history is full (default 1) |

`scope read` defaults to the whole capture of the last `scope status` (all channels,
`start` = first sample, `count` = pre + post) and to a file named
`scope_<unix time>.csv`. The CSV has the columns `sample` (index relative to the
trigger), `time_s` and `ch<N>` (raw 12-bit ADC codes).

The download is resent by range. DATA frames are placed by channel and sample offset.
When the transfer completes (or stalls for 3 s), every missing range is requested again,
for up to 3 rounds. The status panel shows the progress and the resend round. If samples
are still missing after that, they are left empty in the CSV and the log says how many.

## Aux data link

The control link carries commands and, by default, the scope data too. For long
captures, the aux UART can carry the DATA frames while commands stay on the control
link, like FTP's separate control and data connections.

1. Start with `--aux-port /dev/ttyUSB1 --aux-baud <rate>`, where the rate is the one the device uses (the CubeMX USART setting after reset).
2. Optionally raise the rate: `link baud 2000000`. The device switches its aux UART and
   reports the rate it actually achieved (`Data link: device aux UART at 2000000 Bd`), and
   hostctl then switches the host port to the requested rate.
3. `scope read` now uses `link=aux` automatically; `link=origin` forces the control link.

`link status` shows frame, CRC and header-error counters for both links. They are useful
for finding the highest reliable aux baud rate.

## Configuration limits

Requests outside these limits are rejected before anything is written to the hardware.

| Setting | Range |
|---|---|
| Charge voltage limit (`vchg`) | 3.0 – 18.8 V |
| Charge current limit (`ichg`) | 0.05 – 5.0 A |
| Input voltage limit (`vin`) | 3.6 – 22.0 V |
| Input current limit (`iin`) | 0.1 – 3.3 A |
| Output voltage (`v`) | TPS55289 range set by its feedback divider |
| Output current limit (`ilim`) | `ilim × rsense` ≤ 63.5 mV |
| Scope channels | 1 – 4 |
| Aux baud rate | up to f<sub>USART</sub>/8 |

## Hardware emulator

`hostctl -e` runs an in-process emulator in place of the serial port. It uses the same
frame encoding and parser as a real link. It emulates:

- charger and output regulator status, measurements with noise, and config/enable
  requests (partial updates, as on the device);
- applets (`aux-pwm` mock);
- the oscilloscope: `scope config/arm/trigger/stop/status/read`. A capture contains
  phase-shifted sines (period 200 samples). Reads produce real DATA frames and a
  ScopeReadComplete event. Tests can drop chosen DATA frames to exercise resend by range;
- `link baud` (DataLinkStatus);
- `log` (a short emulated boot log) and `info`;
- the IINDPM charger flag while charging.

DAC requests are answered with `unsupported command`, as on the device.

## Tests

| Test | Covers |
|---|---|
| `test_frame` | Frame encoding, CRC, resynchronisation, truncated frames |
| `test_scope_core` | Oscilloscope ring/PSRAM bookkeeping |
| `test_commands` | hostctl Connector + emulator end to end: ping, status, config, enable, partial config, DAC unsupported, scope capture, read with dropped frames and resend, CSV, out-of-range read, log subscription with level filter, device info |

## Troubleshooting

| Symptom | Check |
|---|---|
| Something fails on the device but nothing is logged | `log debug`; the last 16 messages since boot are replayed on subscribe |
| Random resets | `info` shows the reset cause (IWDG = watchdog not kicked, BOR = supply dip) |
| `Ping: never` | Port and baud; the control UART and USB CDC both accept commands |
| `Controller: failure - HAL busy` | The controller already has two requests queued; retry |
| `scope read: no finished capture known` | Run `scope status` after the capture is done, or give `ch=`, `start=`, `count=` |
| Read stalls on `link=aux` | `--aux-baud` must match the device rate; check `link status` CRC/header errors |
| `Link err crc …` grows | Electrical noise or a baud mismatch on the control link |
