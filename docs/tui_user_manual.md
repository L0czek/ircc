# TUI HostCTL User Manual

## Table of Contents
1. [Overview](#overview)
2. [Getting Started](#getting-started)
3. [Command-Line Interface](#command-line-interface)
4. [Main Interface](#main-interface)
5. [Navigation Controls](#navigation-controls)
6. [Command Mode](#command-mode)
7. [Menu Systems](#menu-systems)
8. [Protocol Reference](#protocol-reference)
9. [Troubleshooting](#troubleshooting)

---

## Overview

The TUI HostCTL (Terminal User Interface Host Controller) is a terminal-based application for monitoring and controlling the Resonance Converter Controller. It provides a real-time interface for:

- Monitoring charger status and measurements
- Managing BackBoost (TPS55289) operation
- Configuring and monitoring DAC (Digital-to-Analog Converter)
- Executing custom applet commands
- Viewing system logs and diagnostics

The application uses protocol buffers over a serial connection to communicate with the embedded controller.

---

## Getting Started

### Prerequisites

- STM32 Resonance Converter Controller with USB-C DCDC PD enabled
- USB-to-Serial adapter connected to the debug port
- Linux, macOS, or Windows host system

### Installation

Build the hostctl application and emulator:

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

The binaries will be located in `build/host/`:
- `hostctl.elf` - Main TUI application
- `test_emulator.elf` - Hardware emulator for testing

### Running with Physical Hardware

```bash
./hostctl --port /dev/ttyUSB0 --baud 115200
```

### Running with Hardware Emulator

For development and testing without physical hardware, use the built-in hardware emulator:

**Step 1: Start the emulator**

```bash
cd build/host
./test_emulator --packet-log
```

The emulator will display:
- Initial state (VBAT: 0.00 V, I: 0.00 A)
- Periodic status updates
- Packet exchange logs (when `--packet-log` is used)

**Step 2: Connect TUI to emulator**

Since the emulator runs in-process, modify `host/src/connector.cc` to use `EmulatorConnector` instead of `boost::asio::serial_port`:

```cpp
#include "emulator_connector.hpp"

// Create emulator
HardwareEmulator emulator([](const std::string& msg) {
    BOOST_LOG_TRIVIAL(info) << "[Emulator] " << msg;
}, packet_log ? packet_log_callback : nullptr);
emulator.start();

// Use EmulatorConnector instead of serial port
EmulatorConnector connector(io_context, emulator, refresh_callback, add_log);
```

**Alternative: Named Pipe Method**

If you prefer to keep the serial port interface in connector.cc, you can use a modified version that supports named pipes. However, Boost.Asio's serial_port has limitations with FIFOs on some systems.

---

## Command-Line Interface

### Usage

```
hostctl [options]
```

### Options

| Option | Long Form | Description | Default |
|--------|-----------|-------------|---------|
| `-p` | `--port` | Serial port device (required) | N/A |
| `-b` | `--baud` | Baud rate | 115200 |
| `-d` | `--databits` | Number of data bits | 8 |
| `-s` | `--stopbits` | Number of stop bits | 1 |
| `--parity` | | Parity setting | none |
| `-k` | `--keepalive` | Keepalive interval (seconds) | Disabled |
| `-l` | `--log-level` | Logging verbosity | info |
| `--help` | | Display help and exit | N/A |

### Examples

**Basic connection:**
```bash
./hostctl --port /dev/ttyUSB0
```

**With custom baud rate:**
```bash
./hostctl --port /dev/ttyUSB0 --baud 57600
```

**With keepalive:**
```bash
./hostctl --port /dev/ttyUSB0 --keepalive 30
```

**Verbose logging:**
```bash
./hostctl --port /dev/ttyUSB0 --log-level trace
```

---

## Main Interface

The TUI displays three main components arranged vertically:

```
+----------------------------------------+
| Status Bar                             |
+----------------------------------------+
| Log Buffer (20 lines)                  |
+----------------------------------------+
| Command Input / Menu Components        |
+----------------------------------------+
```

### Status Bar

The top line shows:
- **Current mode**: Normal, Command Input, or active menu (Charger/BackBoost/DAC)
- **Last ping**: Time since last successful communication with controller
- **Heartbeat**: ● (active) or ○ (inactive) indicator

### Log Buffer

The middle section displays the last 20 log entries in reverse chronological order (newest at bottom):
- System initialization messages
- Command execution results
- Error messages
- Protocol communication details

### Command Input Area

The bottom section shows:
- In **Normal mode**: Prompt to press `:` for command mode
- In **Command mode**: `: <your input>` with cursor
- In **Menu mode**: Relevant status and configuration displays

---

## Navigation Controls

### Focus Navigation (Normal Mode)

| Key | Action |
|-----|--------|
| `↑` | Move focus to previous component (Status ← Log ← Command) |
| `↓` | Move focus to next component (Status → Log → Command) |

Focus is indicated by a heavy border around the component.

### General Controls

| Key | Action |
|-----|--------|
| `:` | Enter command input mode |
| `Esc` | Cancel current operation / exit menu |
| `Enter` | Confirm selection / execute command |
| `Backspace` | Delete character in command mode |

---

## Command Mode

Enter command mode by pressing `:` in normal mode. The prompt changes to `: ` where you can type commands.

### Available Commands

| Command | Description |
|---------|-------------|
| `ping` | Send ping request to controller |
| `config:` | Enter Charger Menu |
| `bbconfig:` | Enter BackBoost Menu |
| `dacconfig:` | Enter DAC Menu |

### Command Syntax

**Ping:**
```
:ping
```
Response: Pong message from controller.

**Charger Configuration:**
```
:config:
```
This enters the Charger Menu where you can view and edit charger settings.

**BackBoost Configuration:**
```
:bbconfig:
```
This enters the BackBoost Menu.

**DAC Configuration:**
```
:dacconfig:
```
This enters the DAC Menu.

### Command Notes

- Commands are case-sensitive
- Press `Enter` to execute
- Press `Esc` to cancel and return to normal mode
- Command history is not preserved between sessions

---

## Menu Systems

All three menu systems (Charger, BackBoost, DAC) share the same navigation pattern.

### Menu Navigation

| Key | Action |
|-----|--------|
| `↑` | Move selection up |
| `↓` | Move selection down |
| `Enter` | Execute selected action |
| `Esc` | Exit menu |

### Menu Options

Each menu contains these options (in order):

1. **Refresh Status** - Request current status from controller
2. **Refresh Measurements** - Request current measurements from controller
3. **Refresh Config** - Request current configuration from controller
4. **Edit Config** - Enter configuration input mode
5. **Exit** - Return to normal mode

---

### Charger Menu

The Charger Menu displays:

**Status Information:**
- Charger enabled status
- Input plugged status
- Battery present status
- Fault present status
- Charge status (Not Charging, Trickle, Precharge, Fast, Taper, Top Off, Done)
- VBUS status (USB SDP, USB CDP, USB DCP, Adjustable, Unknown, etc.)
- Input source (USB DPDM, AC1, AC2, Auto)

**Measurements:**
- Battery voltage (VBAT)
- VBUS voltage
- System voltage (VSYS)
- Charge current (IBUS)

**Configuration Fields:**
```
Charge Voltage (V): [12.0] (3.5-19.2V)
Charge Current (A): [2.0] (0-12.79A)
Input Voltage (V): [13.5] (3.9-25.5V)
Input Current (A): [2.5] (0-12.79A)
```

**Charger Input Source:**
- USB DPDM (USB-C DPDM mode)
- AC1 (Primary AC input)
- AC2 (Secondary AC input)
- Auto (Automatic selection)

---

### BackBoost Menu

The BackBoost Menu displays:

**Status Information:**
- Output enabled status
- Fault present status
- SCP Fault (Short Circuit Protection)
- OCP Fault (Over Current Protection)
- OVP Fault (Over Voltage Protection)
- Operating Mode (Buck, Boost, Buck-Boost, Frequency Limit)
- Feedback Mode (PWM, PFM)

**Measurements:**
- Output voltage
- Output current

**Configuration Fields:**
```
Output Voltage (V): [12.0] (5-24V)
Output Current (A): [3.0] (0-10A)
```

---

### DAC Menu

The DAC Menu displays:

**Status Information:**
- DAC enabled status
- Mode (Voltage Mode, Waveform Mode)
- Waveform type (Sine, Square, Triangle, Sawtooth, Custom)
- Frequency (Hz)
- Amplitude (V)
- Error status
- Waveform error status

**Measurements:**
- Voltage output

**Configuration Fields:**
```
Mode: [0] (0=Voltage, 1=Waveform)
Voltage (V): [1.65] (0-3.3V)
Waveform Type: [0] (0=Sine, 1=Square, 2=Triangle, 3=Sawtooth, 4=Custom)
Frequency (Hz): [1000] (0.1-100000)
Amplitude (V): [1.65] (0-3.3)
```

**Waveform Types:**
- 0 = Sine
- 1 = Square
- 2 = Triangle
- 3 = Sawtooth
- 4 = Custom

---

## Protocol Reference

The TUI communicates with the controller using Protocol Buffers over UART.

### Basic Commands

| Message | Direction | Description |
|---------|-----------|-------------|
| `Ping` | Host → Controller | Ping request |
| `Pong` | Controller → Host | Ping response |

### Charger Commands

| Message | Direction | Description |
|---------|-----------|-------------|
| `GetChargerStatus` | Host → Controller | Request charger status |
| `GetChargerMeasurements` | Host → Controller | Request measurements |
| `GetChargerConfig` | Host → Controller | Request configuration |
| `ChargerConfigRequest` | Host → Controller | Set configuration |

**Charger Status Fields:**
- `enabled` (bool)
- `plugged_in` (bool)
- `charge_status` (enum)
- `vbus_status` (enum)
- `battery_present` (bool)
- `fault_present` (bool)
- `input_source` (enum)

**Charge Status Values:**
- 0 = NOT_CHARGING
- 1 = TRICKLE_CHARGE
- 2 = PRECHARGE
- 3 = FAST_CHARGE
- 4 = TAPER_CHARGE
- 6 = TOP_OFF
- 7 = CHARGE_DONE

**VBUS Status Values:**
- 0 = NO_INPUT
- 1 = USB_SDP_500MA
- 2 = USB_CDP_1500MA
- 3 = USB_DCP_3250MA
- 4 = ADJUSTABLE_HIGH
- 5 = UNKNOWN_ADAPTER
- 6 = NON_STANDARD_ADAPTER
- 7 = OTG_MODE
- 8 = NOT_QUALIFIED_ADAPTER
- 11 = POWERED_FROM_VBUS

**Charger Input Source Values:**
- 0 = USB_DPDM
- 1 = AC1
- 2 = AC2
- 3 = AUTO

### BackBoost Commands

| Message | Direction | Description |
|---------|-----------|-------------|
| `GetBackBoostStatus` | Host → Controller | Request status |
| `GetBackBoostMeasurements` | Host → Controller | Request measurements |
| `GetBackBoostConfig` | Host → Controller | Request configuration |
| `BackBoostConfigRequest` | Host → Controller | Set configuration |

**BackBoost Status Fields:**
- `enabled` (bool)
- `fault_present` (bool)
- `scp_fault` (bool)
- `ocp_fault` (bool)
- `ovp_fault` (bool)
- `operating_mode` (enum)
- `feedback_mode` (enum)

**Operating Mode Values:**
- 0 = BUCK
- 1 = BOOST
- 2 = BUCK_BOOST
- 3 = FREQUENCY_LIMIT

**Feedback Mode Values:**
- 0 = PWM
- 1 = PFM

### DAC Commands

| Message | Direction | Description |
|---------|-----------|-------------|
| `GetDACStatus` | Host → Controller | Request status |
| `GetDACMeasurements` | Host → Controller | Request measurements |
| `GetDACConfig` | Host → Controller | Request configuration |
| `DACConfigRequest` | Host → Controller | Set configuration |

**DAC Status Fields:**
- `enabled` (bool)
- `mode` (enum)
- `waveform_type` (enum)
- `frequency` (float)
- `amplitude` (float)
- `error` (bool)
- `waveform_error` (bool)

**DAC Mode Values:**
- 0 = VOLTAGE
- 1 = WAVEFORM

**Waveform Type Values:**
- 0 = SINE
- 1 = SQUARE
- 2 = TRIANGLE
- 3 = SAWTOOTH
- 4 = CUSTOM

### Applet Commands

| Message | Direction | Description |
|---------|-----------|-------------|
| `AppletCommand` | Host → Controller | Execute applet command (start/stop args for lifecycle) |
| `AppletStatus` | Controller → Host | Applet status response |

---

## Troubleshooting

### Common Issues

**1. Cannot open serial port**

```
Error: failed to open serial port
```

**Solutions:**
- Verify the port exists: `ls /dev/ttyUSB*` or `ls /dev/ttyACM*`
- Check permissions: `sudo usermod -a -G dialout $USER`
- Verify no other process is using the port: `lsof /dev/ttyUSB0`
- Check USB connection and device detection: `dmesg | grep tty`

**2. No communication with controller**

- Ensure the controller is powered on
- Verify the USB-C PD is enabled on the controller
- Check that the controller firmware is flashed with USB-C DCDC support
- Try different baud rates (115200, 57600, 9600)
- Check wiring: TX→RX, RX→TX, GND→GND

**3. Stuck in command mode**

Press `Esc` to cancel and return to normal mode.

**4. Menu not responding**

- Ensure you're in the correct mode (Normal or Command Input)
- Try pressing `Esc` twice to ensure clean exit
- Check that the controller is sending responses (watch the log buffer)

**5. Stale data in display**

Press the appropriate "Refresh" option in the relevant menu, or press `Enter` to refresh the status bar.

---

## Quick Reference Card

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `↑` | Focus prev / Menu up |
| `↓` | Focus next / Menu down |
| `:` | Enter command mode |
| `Esc` | Cancel / Exit menu |
| `Enter` | Execute / Confirm |
| `Backspace` | Delete char (command mode) |

### Commands

| Command | Purpose |
|---------|---------|
| `ping` | Test connection |
| `config:` | Charger settings |
| `bbconfig:` | BackBoost settings |
| `dacconfig:` | DAC settings |

### Configuration Limits

| Parameter | Min | Max | Unit |
|-----------|-----|-----|------|
| Charge Voltage | 3.5 | 19.2 | V |
| Charge Current | 0 | 12.79 | A |
| Input Voltage | 3.9 | 25.5 | V |
| Input Current | 0 | 12.79 | A |
| Output Voltage (BackBoost) | 5 | 24 | V |
| Output Current (BackBoost) | 0 | 10 | A |
| DAC Voltage | 0 | 3.3 | V |
| DAC Frequency | 0.1 | 100000 | Hz |

---

## Hardware Emulator Guide

### Overview

The hardware emulator simulates the STM32 resonance converter controller, allowing you to develop and test the TUI without physical hardware. It provides realistic behavior including:

- Protocol buffer message encoding/decoding
- Realistic measurements with noise and drift
- Packet exchange logging
- Dynamic state changes

### Starting the Emulator

```bash
cd build/host
./test_emulator [options]
```

**Options:**
- `--packet-log` - Enable packet exchange logging (last 50 packets)

**Example:**
```bash
./test_emulator --packet-log
```

### Emulator Output

The emulator displays:
```
========================================
  Hardware Emulator Test Program
========================================

[Emulator] HardwareEmulator initialized
[Emulator] HardwareEmulator started
Initial State:
  Charger Enabled: No
  Battery Voltage: 0 V
  Charge Current:  0 A

========================================
  To test with TUI, run:
  1. Start this emulator: ./test_emulator --packet-log
  2. In another terminal, start hostctl with a named pipe:
     mkfifo /tmp/emulator_serial
     ./hostctl --port /tmp/emulator_serial --baud 115200

  Or modify connector.cc to use the emulator directly.
========================================

Emulator running. Press Ctrl+C to stop...
[Status] VBAT: 0.00 V, I: 0.00 A
```

### Emulator States

The emulator maintains these subsystem states:

**Charger:**
- Enabled/Disabled
- Input source selection (USB DPDM, AC1, AC2, Auto)
- Configuration (voltage, current limits)
- Measurements (VBAT, IBUS, VBUS with realistic noise)

**BackBoost (Output Voltage Regulator):**
- Enabled/Disabled
- Operating mode (Buck, Boost, Buck-Boost)
- Configuration (output voltage, current limit)
- Measurements (output voltage, current with load simulation)

**DAC:**
- Mode (Voltage, Waveform)
- Waveform type (Sine, Square, Triangle, Sawtooth)
- Frequency and amplitude settings
- Output voltage monitoring

### Testing Workflow

1. **Start the emulator:**
   ```bash
   cd build/host
   ./test_emulator --packet-log > /tmp/emulator_output.log 2>&1 &
   EMULATOR_PID=$!
   ```

2. **Modify connector.cc** to use `EmulatorConnector` (see Step 2 in "Running with Hardware Emulator")

3. **Build and run hostctl:**
   ```bash
   cd build
   cmake ..
   make -j$(nproc)
   ./hostctl --port /dev/null  # Port is ignored when using emulator
   ```

4. **View emulator logs:**
   ```bash
   cat /tmp/emulator_output.log
   ```

5. **Stop the emulator:**
   ```bash
   kill $EMULATOR_PID
   ```

### Packet Logging

Enable packet logging with `--packet-log` to see protocol communication:

```
[Status] VBAT: 12.02 V, I: 2.03 A
[Status] VBAT: 12.01 V, I: 2.01 A
[Status] VBAT: 12.03 V, I: 2.04 A

========================================
  Packet Exchange Log (Last 50)
========================================
Wed Oct  1 15:30:45 2025 TX -> GET_CHARGER_STATUS
Wed Oct  1 15:30:45 2025 RX <- GET_CHARGER_STATUS
Wed Oct  1 15:30:45 2025 TX -> GET_CHARGER_MEASUREMENTS
Wed Oct  1 15:30:45 2025 RX <- GET_CHARGER_MEASUREMENTS
```

### Troubleshooting Emulator

**Emulator doesn't start:**
- Verify build: `ls build/host/test_emulator.elf`
- Check dependencies: `ldd build/host/test_emulator.elf`

**No state updates:**
- Ensure emulator is running: `ps aux | grep test_emulator`
- Check log output for errors

**Connection issues:**
- The emulator must be integrated into hostctl via `EmulatorConnector`
- Direct named pipe connections have Boost.Asio limitations on some systems
- Use `--packet-log` to verify packet exchange

---

## License

This documentation is part of the Resonance Converter Controller project.
