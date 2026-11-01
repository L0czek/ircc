# Hardware Emulator

The hardware emulator simulates the STM32 resonance converter controller, allowing you to test the TUI without physical hardware.

## Features

- **Protocol Buffer Emulation**: Full support for all protocol messages
- **Realistic Hardware Behavior**: Simulates measurements with noise and drift
- **Packet Exchange Logging**: View all protocol exchanges between host and emulator
- **Configurable State**: Set charger, BackBoost, and DAC configurations
- **Threading**: Runs in background thread for async behavior simulation

## Building

```bash
cd build
cmake ..
make hostctl test_emulator
```

## Usage

### Option 1: Direct TUI Integration (Recommended)

Modify `host/src/main.cc` to use the emulator instead of the serial connector:

```cpp
#include "emulator_connector.hpp"
#include "hardware_emulator.hpp"

int main(int argc, char **argv) {
    // ... existing CLI parsing code ...
    
    // Create emulator
    HardwareEmulator emulator;
    emulator.start();
    
    // Create emulator connector
    EmulatorConnector connector(io_ctx, emulator, 
        std::bind(&Tui::refresh, &interface),
        std::bind(&Tui::add_log, &interface, std::placeholders::_1));
    
    // Use connector instead of serial port
    // ... rest of initialization ...
    
    // Keep running
    interface.run();
    
    // Cleanup
    emulator.stop();
    return 0;
}
```

### Option 2: Test Program

Run the standalone test program:

```bash
./test_emulator --packet-log
```

This shows:
- Initial hardware state
- Real-time measurements
- Packet exchange log on exit

### Option 3: Named Pipe (Advanced)

Create a named pipe and use it as a serial port:

```bash
# Terminal 1 - Start emulator
./test_emulator --packet-log

# Terminal 2 - Start TUI with the emulator
./hostctl --port /dev/ttyUSB0 --baud 115200
```

Note: This requires modifying `connector.cc` to use a custom connector that routes to the emulator.

## Emulated Hardware State

### Charger
- **Default**: Disabled, no battery
- **Measurements**: 12.0V VBAT, 2.0A charge current (with ±50mV noise)
- **Config**: 12V charge limit, 2A charge current, 13.5V input limit

### BackBoost (TPS55289)
- **Default**: Disabled, Buck mode
- **Measurements**: 12.0V output, 1.5A output (with ±500mA noise)
- **Config**: 12V output, 3A current limit, 0.005Ω sense resistor

### DAC
- **Default**: Disabled, Voltage mode, 1.65V
- **Waveform**: Sine wave, 1kHz, 1.65V amplitude
- **Config**: 1.65V output, 1kHz frequency, 1.65V amplitude

## Protocol Messages

The emulator handles all protocol messages:

### Charger
- `GetChargerStatus` → Returns simulated charger status
- `GetChargerMeasurements` → Returns measurements with noise
- `GetChargerConfig` → Returns configured limits
- `SetChargerConfig` → Updates configuration

### BackBoost
- `GetBackBoostStatus` → Returns operating mode based on load
- `GetBackBoostMeasurements` → Returns output voltage/current
- `GetBackBoostConfig` → Returns configured limits
- `SetBackBoostConfig` → Updates configuration

### DAC
- `GetDACStatus` → Returns mode and waveform settings
- `GetDACMeasurements` → Returns voltage output
- `GetDACConfig` → Returns configured values
- `SetDACConfig` → Updates configuration

### System
- `Ping` → Returns `Pong` response

## Packet Logging

Enable packet logging to see all protocol exchanges:

```cpp
HardwareEmulator emulator(
    nullptr,  // No log callback
    [](const std::string& msg) { 
        std::cout << msg << std::endl;  // Print packets
    }
);
```

Or use the test program with `--packet-log` flag.

## Extending the Emulator

To add new features:

1. **Add new state variables** in `HardwareEmulator::State`
2. **Initialize default values** in the constructor
3. **Add getter/setter methods** in the header
4. **Implement command handlers** in `handle_*` methods
5. **Add state update logic** in `update_hardware_state()`

## Limitations

- No actual PWM generation (simulated measurements only)
- USB-C PD is simulated at the status level only (an attached 5/9/15/20 V 3 A source and the contract the firmware policy would pick), not at the protocol level
- No applet execution (returns placeholder responses)
- Fixed response timing (10-50ms delay)

## Troubleshooting

**No response from emulator:**
- Ensure `emulator.start()` is called
- Check that `process_response()` is being called
- Verify packet logging shows commands are received

**Stale measurements:**
- measurements update every 100ms in the background thread
- Call `refresh()` after sending requests
- Check for mutex lock issues in handler methods

## License

Part of the Resonance Converter Controller project.
