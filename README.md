# Resonant Converter Controller

This repository contains a work-in-progress resonant converter controller project based on the STM32G474 microcontroller. The system splits responsibilities between the MCU (`controller`) and a host application (`host`) running on an x86 machine for configuration, logging, and waveform retrieval.

**Goals**
- Implement a phase-shifted resonant converter controller using the STM32G474 high-resolution timers.
- Provide over-current and other protections using built-in ADCs and comparator peripherals.
- Log waveforms to local SRAM for later retrieval by the host.
- Provide a PC-side host binary to configure and interact with the MCU.

Repository layout
- [controller](controller): MCU firmware (STM32 HAL-based). Core control logic lives here.
- [host](host): Host-side application that communicates with the MCU (x86 binary).
- [target](target): CubeMX / board-specific projects and HAL integration for different targets/boards.
- [pcb](pcb): KiCad PCB design files for the controller hardware.
- [protocol](protocol): Protocol definitions and protobufs used by host/MCU.
- [external](external): Third-party libraries and dependencies (e.g., nanpb, drivers).
- [build](build): Top-level build artifacts and CMake cache (generated during builds).

Quick start

Build host part (x86):

1. Enter the host directory and create a build directory:

```bash
cd host
mkdir -p build && cd build
cmake ..
make -j
```

The produced host binary will be in `host/build` (or as specified by the host CMakeLists).

Build MCU firmware:

1. Edit or inspect the CubeMX project in the appropriate `target/*` folder (for your chosen board). Generate code from CubeMX if you modify pin/peripheral configuration.
2. Use CMake from the top-level CMake to build the firmware. Example (from project root):

```bash
mkdir -p build-mcu && cd build-mcu
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake ..
make -j
```

Flashing and debugging
- An OpenOCD config is available at `build/openocd.cfg` for target debugging.
- Typical workflow using the repository make targets:

	1. Start OpenOCD to provide the debug server:

		 ```bash
		 make openocd
		 ```

	2. In a separate terminal, flash or debug the firmware:

		 - To flash and run the firmware:

			 ```bash
			 make run
			 ```

		 - To spawn `gdb` for interactive debugging (connects to the OpenOCD server):

			 ```bash
			 make gdb
			 ```

- Or use your preferred OpenOCD/ST-Link workflow if you have a different setup.

Protocol and message formats
- Communication between host and MCU uses a UART serial link and the message definitions in the [protocol](protocol) directory (protobuf/nanopb). Regenerate or inspect generated code if you change `.proto` files.

Design notes
- The controller firmware uses the STM32G474 high-resolution timers to implement phase-shift control for MOSFET/IGBT bridge switching.
- ADCs, comparators, and timers are used for protections, sampling, and logging to SRAM.

Acknowledgements
- Third-party libraries are located in `external/` — see their respective LICENSE files for details.
