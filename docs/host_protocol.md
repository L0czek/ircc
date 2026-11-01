# Host Protocol

How the host (hostctl or any other client) talks to the controller: links, framing, the
request broker in the firmware, and the oscilloscope bulk-data transfer.

Sources: `protocol/inc/frame.hpp`, `protocol/inc/scope_data.hpp`,
`protocol/src/protocol.proto`, `controller/src/{link,usb_cdc_link,broker,scope_service}.cc`,
`host/src/{connector,scope_client}.cc`.

## Links

| Link | `LinkId` | Accepts requests | Notes |
|---|---|---|---|
| Control UART (`BOARD_CONFIG.message_bus`) | `CtrlUart` | yes | DMA RX/TX when CubeMX provides it, otherwise interrupt mode |
| USB CDC (FS) | `Usb` | yes | Hooks in `usbd_cdc_if.c` USER CODE; a ZLP follows transfers that are a multiple of 64 bytes |
| Aux UART (`g_board_aux_uart`, resonant board) | `AuxUart` | no | Data only; runtime baud rate (`DataLinkConfigure`) |

USB and the control UART are active at the same time. Each request is answered on the
link it arrived on. Events about a transfer go to the link that started it.

## Frame format

Every link carries the same frames (all fields little endian):

| Offset | Size | Field | |
|---|---|---|---|
| 0 | 4 | magic | `"QCW2"`, the resynchronisation point |
| 4 | 1 | type | 1 REQUEST (host→device), 2 RESPONSE, 3 EVENT, 4 DATA |
| 5 | 1 | flags | type specific (DATA: bit 0 = last frame of a channel) |
| 6 | 2 | length | payload bytes, ≤ 2048 |
| 8 | 2 | ~length | rejects a corrupt length at once instead of waiting for bytes |
| 10 | 2 | seq | REQUEST: chosen by the host and echoed in the RESPONSE; EVENT: per-link counter; DATA: frame index within the transfer |
| 12 | N | payload | protobuf `Request` (REQUEST) or `Response` (RESPONSE, EVENT); binary (DATA) |
| 12+N | 4 | crc32 | CRC-32/MPEG-2 over bytes [0, 12+N): poly 0x04C11DB7, init 0xFFFFFFFF, no reflection, no final XOR |

Each frame adds 16 bytes. The CRC matches the STM32 CRC unit's defaults with byte
input; the firmware uses the unit, under a mutex, when CubeMX configures it that way, and
otherwise falls back to the table implementation in `frame.hpp`.

**Parsing.** `frame::Parser` accepts any chunking of the byte stream. It skips bytes until
it finds the magic. It drops one byte and rescans when the header check fails, the type
is invalid, the length is too large, or the CRC is wrong. As a result, a corrupt frame
never hides the frames after it. On the firmware, the RX thread calls `on_idle()` after
20 ms without bytes, so a frame cut off by a host reset is dropped instead of stalling the
parser.

## Firmware broker

```
link RX ring ─► RX thread per request link (frame::Parser)
                 └─ REQUEST ─► free request slot (2 × 2 KB) ─► handler queue
handler thread ─► decode Request ─► MessageHandler::handle() ─► RESPONSE (same seq) on the origin link
                                └─► after_response(): starts a queued scope transfer
```

- Requests are handled one at a time, in arrival order across both links.
- If both slots are taken, the broker answers at once with `RESPONSE_STATUS_FAILURE` and
  `CONTROLLER_ERROR_HAL_BUSY`.
- An undecodable payload is answered with `RESPONSE_STATUS_INVALID_PARAMETERS`.
- Frames that are not REQUESTs are ignored on device links.
- Any thread may send: `send_message()` for RESPONSE/EVENT, `send_frame()` for prebuilt
  DATA frames. Each link serialises its writers under a mutex and chunks large frames.

Every `Response` may carry `result` (`CommandResult{status, error}`). A missing `result`
means success. `error` is the first `ControllerErrorCode` the command hit.

## Requests

| Request (oneof tag) | Response payload | Notes |
|---|---|---|
| `ping` (1) | `pong` | |
| `get_system_status` (2) | `system_status` | charger status, measurements and config; regulator status, setpoints and config |
| `set_charger_config` (3) | `system_status` | optional fields: charge V/I limits, input current cap / VINDPM floor, `dock_input_current_limit`, `input_source`, `enabled` (see below) |
| `set_output_regulator_config` (4) | `system_status` | optional fields: `output_voltage`, `output_current_limit`, `sense_resistor_ohms`, `enabled`; absent fields keep their value; enable is applied after the setpoints |
| `set_dac_config` (5) | — | `UNSUPPORTED_COMMAND` (DAC controller removed) |
| `applet_start/stop/command/list` (6–9) | `applet_status` / `applet_list_response` | |
| `scope_configure` (10) | `scope_status` | see below |
| `scope_arm` (11), `scope_force_trigger` (12), `scope_stop` (13), `scope_get_status` (14) | `scope_status` | |
| `scope_read` (15) | `scope_read_started` | then DATA frames and a `scope_read_complete` EVENT |
| `scope_read_cancel` (16) | — | |
| `data_link_configure` (17) | `data_link_status` | aux UART baud rate |
| `log_subscribe` (18) | — | forward firmware log messages to this link as `log_message` EVENTs |
| `get_device_info` (19) | `device_info` | firmware, uptime, reset cause, heap, threads, PSRAM |
| `set_usb_pd_config` (20) | `system_status` | USB-C: `source_enabled`, `max_sink_voltage` (5–20 V), `max_sink_current` (0.5–3 A), `source_min_battery_voltage` (9–17 V); absent fields keep their value; a changed sink limit renegotiates |
| `usb_pd_renegotiate` (21) | `system_status` | sink: ask for the Source_Capabilities again (`hard_reset` = send a Hard Reset) |

`ChargerStatus.status_flags` and `fault_flags` carry the raw BQ25792 status registers
(REG1B, REG1D, REG1E, REG1F and REG20, REG21; the bit map is in `protocol.proto`). They
show which regulation loop limits charging (IINDPM, VINDPM, TREG, VSYSMIN), expired
safety timers, the TS temperature zone, and the specific fault.
### Input power and USB-C

The firmware's input policy ([usb_pd.md](usb_pd.md)) owns the BQ25792 input current
(IINDPM) and voltage (VINDPM) limits and the input selection: the dock (VAC2) is preferred,
the USB-C input limit comes from the PD contract (or the Type-C Rp current), and the USB-C
port can source 5 V / 3 A from the battery. In `ChargerConfigRequest`:

- `input_current_limit` is a **cap** on any input (0.1–3.3 A, 3.3 = none) and
  `input_voltage_limit` a **floor** for VINDPM (3.6–22 V, 3.6 = none);
- `dock_input_current_limit` (0.1–3.3 A, default 3.0) is the input current on the dock;
- `input_source` `AUTO` means dock first, then USB-C; `AC1` / `AC2` force an input.

`ChargerConfig.input_voltage_limit` / `input_current_limit` report the values in use
(register read-back); `input_current_cap`, `input_voltage_floor` and
`dock_input_current_limit` report the settings.

`SystemStatus.usb_pd` (`UsbPdStatus`) reports the Type-C state, power and data role, CC
line, partner Rp current, the contract (V, A), the partner's Source_Capabilities as raw
32-bit PDOs (`source_pdos`, decoded by hostctl; `selected_pdo` is 1-based), the USB-C
input limit, VBUS (BQ25792 VAC1), the source state (disabled, ready, active, blocked by
the dock or the battery), whether the external dead-battery Rd is connected, and the hard
reset count. `SystemStatus.usb_pd_config` holds the settings. Failures use the
`CONTROLLER_ERROR_USBPD_*` codes (190–195). Boards without UCPD omit both fields and
answer the USB-PD requests with `UNSUPPORTED_COMMAND`.

`ChargerMeasurements` also reports `ibat_current` (+ = charging), `vac1_voltage` (input 1,
USB-C) and `vac2_voltage` (input 2, dock DC). `OutputRegulatorMeasurements` are the
TPS55289 setpoints, because the part has no ADC.

## Diagnostics

**Firmware log.** `printf` output goes nowhere on this board: `__io_putchar` is not
defined, so `_write` discards the characters. Every `os::trace/debug/info/warn/error`
message is therefore also handed to `Diagnostics` (`controller/src/diagnostics.cc`):

- A non-blocking sink copies the message into an 8-entry queue. When the queue is full
  the message is dropped and counted.
- The `log-forwarder` thread (priority BelowNormal) keeps the last 16 messages and sends
  each one as a `LogMessage{tick_ms, level, text, dropped}` EVENT to every link that sent
  `LogSubscribe{enabled, min_level, replay}`.
- With `replay` (the default), the history is sent first, so boot messages such as
  charger/PSRAM init errors or `USART global interrupt disabled` reach the host.
- A link whose send fails is unsubscribed. Messages logged by the forwarder itself are
  never forwarded, so a failing link cannot cause a feedback loop.
- The forwarder's thread, queue and history use static memory, about 5.7 KB of .bss
  including the DeviceInfo task table; nothing comes from the FreeRTOS heap.
- Messages are truncated to 115 characters.

**DeviceInfo** reports:

- board and firmware (`git describe` at CMake configure time, plus the build date);
- uptime;
- FreeRTOS heap free and minimum-ever free;
- `RCC_CSR` reset flags captured at boot (IWDG, WWDG, BOR, pin, software, low-power,
  option bytes);
- PSRAM init result and QUADSPI configuration;
- per thread: name, stack high-water mark in bytes, FreeRTOS priority and state;
- the number of dropped log messages.

Use it to size the FreeRTOS heap and the thread stacks on real hardware.

## Oscilloscope

### Capture

`ScopeConfigure{channels[1..4]{adc, input, sampling_time}, sample_rate_hz,
pre_trigger_samples, post_trigger_samples, trigger}` sets up the injected-conversion
capture. `trigger` (`ScopeTriggerSettings`) enables any combination of:

- `software` (default true);
- `analog_watchdog`, with `awd_channel`, `awd_low` and `awd_high`;
- `comparator` (COMP 1..7);
- `external` (firmware or HRTIM).

`require_full_pretrigger` (default true) ignores triggers until the pre-trigger history
is full.

`ScopeStatus.state` goes IDLE → PRETRIGGER (armed) → POSTTRIGGER → STOPPING → DONE, or
ERROR (with `error`). In DONE, the status describes the capture:

- `num_channels`;
- `sample_rate_hz` (the actual rate);
- `trigger_source`;
- `first_sample`, which is ≤ 0, relative to the trigger;
- `post_trigger_samples`.

The readable sample range is `[first_sample, post_trigger_samples)`.

### Scope read (bulk transfer)

```
host                          control link (origin)                      data link
 │ REQUEST ScopeRead{id, mask, start, count, link}  ─►
 │ ◄─ RESPONSE ScopeReadStarted{id, frames, samples_per_frame, link}
 │                                                   ◄─ DATA × frames  (origin or aux)
 │ ◄─ EVENT ScopeReadComplete{id, frames_sent, cancelled, error}
```

- `transfer_id` is chosen by the host. Its low 16 bits are repeated in every DATA frame.
- `start` is relative to the trigger, and `count` is the number of samples per channel.
  The range must lie inside the capture (`OSCILLOSCOPE_OUT_OF_RANGE`), and `channel_mask`
  must select only existing channels (`OSCILLOSCOPE_INVALID_CHANNEL`).
- Only one transfer runs at a time. A second ScopeRead, ScopeConfigure or
  DataLinkConfigure during a transfer gets `HAL_BUSY`.
- The DATA frames are sent only after the ScopeReadStarted response has gone out, so the
  host always knows the transfer before its data arrives.
- Frames are sent block by block: for each block of 1016 samples, one frame per requested
  channel in ascending order. All channels therefore arrive progressively in time order.
- `ScopeReadCancel{transfer_id}` stops the transfer at the next frame boundary; the
  complete event then has `cancelled = true`.

DATA payload (`scope_data.hpp`):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | transfer_id (low 16 bits) |
| 2 | 1 | channel |
| 3 | 1 | reserved |
| 4 | 4 | sample_offset, relative to `ScopeRead.start` |
| 8 | 2 | sample_count (≤ 1016) |
| 10 | 2 | reserved |
| 12 | 2·n | samples, uint16 right-aligned ADC codes |

A full frame is 16 + 12 + 2032 = 2060 bytes, which is 98.6 % payload.

### Resend by range

Every DATA frame carries its own CRC, channel and offset, so no acknowledgements are
needed. The host places the samples it receives, and after `ScopeReadComplete` (or when
the stream stalls) it issues new ScopeReads for the missing `(channel mask, start,
count)` ranges. The capture stays in PSRAM until the next capture is armed, so any part
can be read again any number of times. hostctl does this for up to 3 rounds, merging
adjacent missing samples into ranges. The frame `seq` (frame index) is only used to
count gaps.

### Aux data link (FTP-style)

Commands and events always use the origin (control) link. With `ScopeRead.link =
DATA_LINK_AUX`, only the DATA frames go to the aux UART. This keeps the control link
responsive and allows a much faster data rate than the control UART:

1. `DataLinkConfigure{baudrate}` reprograms the aux UART. It uses OVER8 above
   f<sub>clk</sub>/16 and accepts rates up to f<sub>clk</sub>/8. The response's
   `DataLinkStatus.baudrate` is the rate actually set from BRR.
2. The host switches its aux port to the requested rate after this response, which
   arrives on the control link.
3. `ScopeRead{…, link: DATA_LINK_AUX}`; the response reports `link`.

Boards without an aux UART answer `UNSUPPORTED_COMMAND`.

Throughput per channel sample, at 10 bits per byte on a UART:

| Link | Rate | ≈ samples/s |
|---|---|---|
| Control UART | 115200 Bd | 5.7 k |
| Aux UART | 2 MBd | 99 k |
| Aux UART | 10 MBd (needs f<sub>USART</sub> ≥ 80 MHz) | 490 k |
| USB CDC FS | ~1 MB/s practical | ~490 k |

## Host implementation (hostctl)

- `Connector`: serial port(s), `frame::Parser` per port, request seq counter, a write
  queue on the io thread, and message/data hooks. Its `feed()` is also used by the
  in-process emulator, so the emulator exercises the real parsing path.
- `ScopeClient`: the read state machine. It handles ScopeReadStarted and
  ScopeReadComplete, places DATA frames, runs a 3 s stall timer, resends by range and
  writes the CSV.
- `HardwareEmulator`: answers every request with real frames, including an emulated
  capture with selectable dropped DATA frames for tests (`test_commands`).
