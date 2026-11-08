# Front panel: OLED display and controls (design)

Status: **implemented, not yet tried on hardware.** It needs the CubeMX changes in §5
first. Open points are at the end.

The front panel is the `resonance_converter_controller_io` board. It plugs into the
controller board through a 20-pin DF12 board-to-board connector (IO J1 ↔ controller J3) and
carries:

- a 1.5" 128×128 OLED, Raystar REX128128AYAP3N00000 (SSD1327, 4-bit grayscale, yellow);
- a 5-way joystick, a rotary encoder with a push switch, and a SHUTDOWN key, all read through a
  PCF8574 I2C expander, except the encoder A/B lines, which go straight to TIM20;
- hardware keys that bypass the MCU: /QON (BQ25792), NRST and HRTIM_EEV9;
- an SWD Tag-Connect.

---

## 1. Hardware analysis

### 1.1 Connector map (IO J1 ↔ controller J3)

The two DF12 halves mate mirrored: controller pin *n* meets IO pin *20−n* when *n* is odd,
and *22−n* when *n* is even. Every net was checked on both sides.

| Signal | MCU pin | Controller J3 | IO J1 | IO side |
|---|---|---|---|---|
| LCD_SCK | PC10 SPI3_SCK | 13 | 7 | J5.15 D0 (SCLK) |
| LCD_MOSI | PC12 SPI3_MOSI | 7 | 13 | J5.16 D1 (SDIN) |
| LCD_SS | PA15 | 9 | 11 | J5.10 CS# |
| LCD_DC | PD1 | 11 | 9 | J5.12 D/C# |
| LCD_RST | PD2 | 16 | 6 | J5.11 RES# **and** U2 TLV61047 EN (VPP boost) |
| LCD_MISO | PC11 | — | — | not routed; not needed (SSD1327 SPI is write-only) |
| ENCODER_CHA | PB2 TIM20_CH1 | 14 | 8 | SW1.A, 10 k pull-up, 100 nF |
| ENCODER_CHB | PE3 TIM20_CH2 | 15 | 5 | SW1.B, 10 k pull-up, 100 nF |
| KEY_INT | PC5 (R77 10 k pull-up) | 3 | 17 | U1 PCF8574 ~INT |
| I2C_INT SCL/SDA | PB8/PB9 I2C1 | 8/10 | 14/12 | U1 PCF8574 (shared with BQ25792, TPS55289, AT24CS32, MCP4441 ×2) |
| /QON | BQ25792 ~QON | 12 | 10 | SW2 (charger ship-mode exit / system reset) |
| NRST | NRST | 1 | 19 | SW3, Tag-Connect reset |
| HRTIM_EEV9 | PB3 HRTIM1_EEV9 | 5 | 15 | SW4 (hardware event input to the HRTIM) |
| SWDIO/SWCLK | PA13/PA14 | 17/19 | 3/1 | J7 Tag-Connect |
| +3.3V | | 2 | 20 | logic, PCF8574, LP5907 2.5 V LDO (OLED VDD) |
| +3.3VP | | 4, 6 | 18, 16 | TLV61047 boost input (OLED VPP) |

### 1.2 PCF8574 (U1) bit map

| Bit | Net | Control |
|---|---|---|
| P0 | DEV_PAD_CENTER | encoder push (SW1.S1) |
| P1 | CENTER | joystick centre |
| P2 | DOWN | joystick down |
| P3 | LEFT | joystick left |
| P4 | RIGHT | joystick right |
| P5 | UP | joystick up |
| P6 | — | unused (leave as input, reads 1) |
| P7 | SHUTDOWN | SW5 |

- All keys are active low, with 100 nF to GND and the PCF8574's weak quasi-bidirectional
  pull-up (~100 µA). That gives a slow RC edge, effectively a hardware debounce.
- ~INT goes low on any input change and clears when the port is read.
- The firmware writes 0xFF once (all quasi-inputs). After that, every access is a 1-byte read.
- **Address:** set by fitting one 0 Ω of each pair R1/R2 (A0), R3/R4 (A1) and R5/R14 (A2).
  In firmware it is a compile-time constant (`kKeypadI2cAddress`, 0x20 to 0x27, in the board
  config).

### 1.3 OLED power and module straps

- **Interface:** BS1 = BS2 = 0 (4-wire SPI). W/R# and RD# are tied to GND and D2 is NC,
  as the datasheet asks.
- **Logic supply:** VCI = +3.3 V, and VDD = +2.5 V from U4 LP5907 (always on). VDD is
  external, so the init code must **disable the internal VDD regulator**: Function Select A
  (0xAB) = **0x00**. The datasheet's sample uses 0x01, which is for its own board.
- **Panel supply (VPP):** U2 TLV61047 boost from +3.3VP. EN is tied to LCD_RST, so the panel
  supply is on exactly while RES# is high. That matches the SSD1327 sequence:
  - power on: VDD/VCI → RES# low → RES# high (VCC rises) → init → display on;
  - power off: display off (0xAE) → RES# low (VCC off).

  While RES# is low, VPP sits at about +3.3VP minus the diode drop through L1/D1, not 0 V.
  That is harmless.
- **VPP setpoint:** FB divider R11 536 k / R12 "60.4" (read as 60.4 k), with V_REF 1.233 V
  (TLV61047), gives **≈12.2 V**. The panel spec is 14–15 V (typ. 14.5 V), and R9 IREF =
  1.15 MΩ was sized for 14.5 V. See the hardware TODO.
- **Power:** typically 24 mA from VPP on a 50 % checkerboard, about 0.35 W from the 3.3 V
  rail. Dark backgrounds and a screen timeout matter on battery.
- **IREF:** R9 1.15 MΩ, the datasheet minimum.

### 1.4 SPI timing

- SSD1327 4-wire SPI: mode 0 (also works in mode 3), MSB first, t_cycle ≥ 100 ns (≤ 10 MHz).
- SPI3 runs from APB1 = 170 MHz:
  - prescaler /32 gives **5.3 MHz** (/16 = 10.6 MHz is over the limit);
  - a full frame is 8192 bytes, so **12.4 ms**, and dirty-row updates are much less.
- D/C# is sampled on the last bit of each byte, so it must not change during a DMA burst.
  Commands and pixel data go out as separate transfers.
- CS on PA15 as a **software GPIO** keeps CS low across command + data. With hardware NSS,
  CS would pulse between transfers. That is legal for the SSD1327, but it adds nothing.

---

## 2. Software architecture

```
 external/SSD1327_STM32_Driver/    submodule: chip driver only, no graphics
   inc/ssd1327.hpp                 Display (init, window, contrast, re-map, power) over a Transport
   inc/ssd1327_hal.hpp             HalTransport: HAL SPI (+ DMA), CS / D/C# / RES# GPIOs
   inc/ssd1327_emulator.hpp        GDDRAM model for host tests and the simulator
 controller/ui/                    panel UI, platform independent (firmware + host)
   inc/ui/gfx.hpp      src/gfx.cc       Framebuffer (4 bpp, panel layout), Canvas, RowTracker
   inc/ui/font.hpp     src/fonts.cc     5x7 / 6x10 / 10x20 fonts (tools/gen_fonts.py)
   inc/ui/input.hpp    src/input.cc     key debounce and events, encoder acceleration
   inc/ui/ui.hpp       src/ui.cc        Ui: screen stack, snapshot, history, alarms, power
   inc/ui/menu.hpp     src/menu.cc      MenuScreen: list + value editor + hold-to-start
   inc/ui/backend.hpp                   what the UI may read / do
   src/dashboard.cc, src/menus.cc       the screens
   tests/test_ui.cc                     host unit tests
 controller/inc/applet_control.hpp   parameter / readout declarations (no HAL)
 controller/src/applet_runner.cc     one applet at a time, shared by host and panel
 controller/src/front_panel.cc       firmware glue: thread, SPI, keypad, backend
 host/src/ui_sim.cc                  simulator: UI + driver + emulator -> PNG screenshots
```

### 2.1 Driver: `external/SSD1327_STM32_Driver`

`ssd1327::Display` holds the chip logic. It talks to an abstract `ssd1327::Transport`
(command bytes, data bytes, asynchronous data, reset, delay), so the same code runs against:
- `HalTransport`: STM32 HAL SPI. It uses DMA when the SPI handle has `hdmatx`, otherwise it
  sends blocking. CS can be a GPIO, or `nullptr` for hardware NSS.
- `Emulator`: a GDDRAM model with the column/row window, re-map, contrast, inverse and reset.

Details:
- `init()` pulses RES#, sends the module init sequence, clears GDDRAM and leaves the display
  off. Function Select A is 0x00, because VDD comes from the external LDO.
- `power_off()` sends 0xAE and holds RES# low, which also turns the panel supply off.
- Re-map: 0x51 for normal, 0x42 for 180°. `Config::swap_nibbles` flips the nibble bit in
  case the left and right pixels of each byte come out swapped on the real panel.
- Host tests: `cmake -S external/SSD1327_STM32_Driver -B <dir> -DSSD1327_BUILD_TESTS=ON`.

### 2.2 Rendering

- `ui::Framebuffer` is 8 KB in the panel's GDDRAM layout (high nibble = left pixel).
- Every frame is redrawn from scratch: clear, status bar, screen, footer, overlays.
- `RowTracker` hashes each row and returns the runs of rows that changed. Only those go to
  the panel, one window + DMA per run. A static screen sends nothing; a value that changes
  sends about 10 rows.
- The burn-in pixel shift is a canvas origin offset of 0/1 px that toggles every minute.

### 2.3 Input

- `keys_from_pcf8574()` maps the port bits.
- `KeyTracker` debounces (10 ms) and emits these events:
  - Press;
  - Release;
  - Click (released before 600 ms);
  - Long (select keys and SHUTDOWN, once at 600 ms);
  - Repeat (navigation keys, after 400 ms, every 110 ms).
- `EncoderAccel` turns detents into `Turn{delta, accel}`. The multiplier is ×10 at ≥ 5 and
  ×100 at ≥ 15 detents per 100 ms. Direction and acceleration are settings.

### 2.4 Data and actions (`ui::Backend`)

- The UI polls `GetSystemStatus` at 4 Hz through `Backend::execute()`, a `Request`/`Response`
  pair. Changes are also `Request`s: `set_output_regulator_config`, `set_charger_config`,
  `set_usb_pd_config` and `usb_pd_renegotiate`.
- On the firmware this is `MessageHandler::handle_local()`: the same code, validation and
  logging as a host request, under the same lock as the broker's handler thread. Link-bound
  requests are refused: scope, data link and log subscription.
- A response that carries `system_status` refreshes the snapshot at once. A failed command
  shows a toast.
- History: one sample per second of VBAT, IBAT, VBUS, IBUS, VOUT and IOUT, 128 samples each
  (`int16` milli-units, 1.5 KB).
- `ChargerConfig.input_source_mode` (new proto field 8) reports the selected input
  (AUTO/AC1/AC2). `ChargerStatus.input_source` is the active one.

### 2.5 Firmware thread (`front_panel.cc`)

- Static thread "panel": osPriorityBelowNormal, 3 KB stack, 25 Hz.
- Each frame:
  1. read the keypad (PCF8574 over `i2c_internal_bus_transaction`), and also on KEY_INT
     (EXTI callback → thread flag);
  2. read the encoder (`board::encoder()->take_delta()`);
  3. `Ui::tick`, render, flush the changed rows;
  4. apply contrast, orientation and power.
- Off: `power_off()`. Wake: `init()` again, then the whole frame is resent.
- A missing panel or keypad is retried every 3 s and shown under System (keypad status,
  display errors).
- Keypad address: CMake cache `FRONT_PANEL_KEYPAD_I2C_ADDRESS` (default 0x20).
- RAM: about 13 KB for the panel object (framebuffer, UI, request/response, history), plus
  the 3 KB stack.

## 3. Interaction design

### 3.1 Controls

| Control | Anywhere | In a list | Editing a value | Dashboard |
|---|---|---|---|---|
| Encoder turn | — | move focus | change value (accelerated) | scroll / next page |
| Encoder push | — | activate / edit | commit | open main menu |
| Joystick ↑ ↓ | — | move focus | ±1 step at the cursor digit | — |
| Joystick ← | — | back | cursor one digit left (coarser) | previous page |
| Joystick → | — | enter submenu | cursor one digit right (finer) | next page |
| Joystick centre | — | activate / edit | commit | open main menu |
| Long centre / long push | **home** | home | **cancel** edit (restore) | home |
| SHUTDOWN click | **soft stop** (applets stopped, VOUT off) | | | |
| SHUTDOWN hold (600 ms) | **power off** (ship mode, §3.7) | | | |

These rules hold on every screen:

- the encoder is the "value" control and the joystick is the "navigation" control;
- ← always means back or cancel;
- a long press always goes home;
- SHUTDOWN always acts, even with the display off or an alarm showing. Only the input test
  and the power off screen take the key themselves.

Live parameters apply while you turn, rate-limited to 10 Hz. Other parameters apply on commit.

### 3.2 Screen layout

```
 ┌────────────────────────────────┐  y 0..8   status bar (5x7 font + 8x8 icons)
 │▮▮▮▯78% ⚡DOCK ⇄5V VO12.0 ▶aux ⚠│
 ├────────────────────────────────┤
 │                                │  y 10..117 body
 │                                │
 ├────────────────────────────────┤
 │ ◀ Power      ●○○○○     USB-C ▶ │  y 119..127 hint/page bar (dim gray)
 └────────────────────────────────┘
```

- **Status bar:**
  - battery level and %, with a charging bolt;
  - input source (USB-C contract V / DOCK / BAT);
  - USB-C role (sink ⇠, source ⇢ with OTG/VOUT);
  - VOUT on/off and setpoint;
  - the running applet;
  - host link connected;
  - a fault icon that blinks on new errors.
- **Gray levels:** bright (15) for values, mid (8) for labels and units, dim (3) for hints
  and grid lines. Backgrounds are black (OLED power).
- **Selection:** an inverted bar (gray 15 on 0, text at 0). While editing, the value
  flashes and the cursor digit is underlined.

### 3.3 Home: dashboard pages (← / → or encoder)

**1. Overview**
```
 BAT  15.62V  +1.20A  78%
 IN   USB-C 20V 3A  (DOCK —)
 VOUT ■ 12.00V  0.84A  CV
 USB  SINK 20.0V 2.25A
 ─────────────────────────
 ▶ aux-pwm   RUNNING  00:42
   f 125.0kHz  D 50/50 %
 TDIE 41°C   FAN 35%
```

**2. Power (charger):**
- VBUS, IBUS, VBAT, IBAT, VSYS, VAC1/VAC2, TS %, TDIE;
- charge state;
- the active input and limits (IINDPM/VINDPM), with a ● when it is regulating;
- faults.

**3. VOUT:**
- big 12×24 digits for the measured voltage, with the setpoint, current and limit below;
- CV/CC, faults;
- a sparkline of IOUT.

**4. USB-C:** attach state, power and data role, contract, source path
(NONE/OTG/VOUT/BLOCKED_*), CC line, PDO list.

**5. Trends:** two sparklines, chosen with the encoder (VBAT+IBAT, VOUT+IOUT, TDIE),
128 s window, autoscaled, min/max labels.

**6. Applet** (shown only while one runs): the applet's run screen (§3.5).

### 3.4 Main menu (centre or encoder push from home)

```
 Applets          ▶     registered applets with their state → applet page
 Outputs          ▶     VOUT: output, voltage, current limit (+ measured, mode)
                        Charger: charging, charge V/I, input (Auto/USB-C/Dock), input cap,
                                 input min V, dock limit (+ IINDPM, VINDPM)
                        USB-C: source role, max sink V/I, source min battery, renegotiate
 Measurements     ▶     every measurement, read-only list
 System           ▶     firmware, board, uptime, host link, free heap, I2C recoveries,
                        keypad, display errors
 Settings         ▶     contrast (live), dim after, off after, rotate 180, encoder reverse /
                        acceleration, pixel shift, input test, display test
```

VOUT voltage and the charger limits apply on commit (click), not while turning.

### 3.5 Applets

**Applet page** (from the Applets list):
```
 aux-pwm            ● STOPPED
 ───────────────────────────
 ▶ Run
   Frequency    125.00 kHz
   Duty A          50.0 %
   Duty B          50.0 %
   Channels      A+B
   Presets          ▶
   About            ▶
```

- **Run → run screen:**
  - the applet's readouts in large digits;
  - the encoder adjusts the "primary live" parameter directly (for example frequency),
    and → / ← pick which live parameter the encoder controls;
  - "Stop" is the default focused item, so one centre press stops.
- **Parameters** that are not live while running are greyed out (they can't be changed
  while the applet is running).
- **Presets:** save, load or delete up to N named presets per applet in the EEPROM, using
  the existing `config_size()/save_config()/load_config()`.

**Applet framework.** An applet declares its parameters and readouts once, in
`AppletControl` (`controller/inc/applet_control.hpp`, no HAL types). `Applet` derives from it
and keeps `configure`/`on_message`/`save_config`/`load_config`.

```cpp
struct ParamDesc {
    const char* key;      // CLI name: "freq"
    const char* label;    // panel label: "Frequency"
    const char* unit;
    ParamType type;       // Number, Bool, Choice
    float min, max, step;
    uint8_t decimals;
    std::span<const char* const> choices;
    uint8_t flags;        // kParamLive, kParamPersist, kParamReadOnly
};
class AppletControl {
    name(), summary(), params(), get_param(i), set_param(i, v), readouts(), readout(i),
    run_state() (Stopped / Running / Fault), fault_text(), start(), stop(), needs_arming()
};
```

- `AppletRunner` (`controller/src/applet_runner.cc`) owns start and stop for both the host
  and the panel:
  - one applet at a time: starting one stops the other;
  - one recursive mutex, so a panel edit never interleaves with a host command;
  - range checks with `check_param()`;
  - a 10 ms thread that calls `step()` of the running applet.
- Generic host commands use the declarations:
  - `applet_command <name> params`
  - `applet_command <name> get [key]`
  - `applet_command <name> set <key> <value>`
- `applet_start <name> [args]` now runs `configure(args)` (if any args) and then **starts**
  the applet, the same as the host emulator already did.
- aux-pwm:
  - ported: freq, duty1 and duty2 are all live parameters;
  - argum is no longer used (it added about 200 KB of flash at -O0);
  - the frequency is now really programmed (prescaler + ARR from the timer clock);
  - the applet is now registered at boot. Before, `AuxPwmAppletRegistration` was never
    instantiated, so no applet existed.

### 3.6 Alarms and dialogs

- **Faults** (charger, VOUT, USB-PD error, applet fault, HRTIM fault/EEV9):
  - a modal toast with the source and text;
  - centre to acknowledge, after which the status bar fault icon stays until it clears.
- **Confirm dialogs** for disruptive actions: power role swap, presets overwrite.
- **Hold to start:** to start an applet, hold centre or the encoder for 1 s while a
  progress bar fills; releasing early cancels. Stopping is always one press (SHUTDOWN
  click stops everything).
- **Host activity:** the panel and host are equal peers (last writer wins). When the host
  changes something the panel is showing, the value updates live. If the panel is editing
  that exact value, the edit is cancelled and a short "changed by host" toast shows.

### 3.7 Power off

The SHUTDOWN key (IO SW5) sends the host request `power_off`. The host can send the same
request.

**Click: soft stop.** All applets stop and VOUT turns off. The board stays on, charging
continues, and the USB-C settings are not touched. A toast says "Outputs stopped".

**Hold: power off.**
1. The firmware does the soft stop.
2. PowerPath turns the USB-C source off for as long as the power off runs.
3. The panel opens the power off screen, which lists USB-C (VAC1) and the dock (VAC2),
   each marked plugged or unplugged.
4. Once both are unplugged, PowerPath turns the 6 kΩ VBUS pull-down on. When VBUS has
   dropped, it writes `SDRV_CTRL = ship mode` with the 10 s delay and reads it back.
5. The screen counts down "Off in N s". At zero the charger turns ship FET Q3 off and the
   battery is disconnected.

Details:
- The charger refuses ship mode while it still sees VBUS. PowerPath then retries every
  500 ms.
- If power is plugged in during the count-down, the charger leaves ship mode by itself and
  the screen goes back to the list.
- To turn the board on again, press /QON: SW1 on the controller board or SW2 on the IO
  board. Plugging in an adapter also turns it on.
- Cancel with ◀, select or a SHUTDOWN click. During the count-down, cancelling also writes
  `SDRV_CTRL = IDLE`.
- The display stays on while this screen is open. The screen closes by itself when the
  host cancels.
- `SFET_PRESENT = 1` is written at boot (`CHARGER_SHIP_FET_PRESENT`, default 1). Without
  it the charger ignores ship-mode writes.

States, reported in `system_status.power_off`:

| State | Meaning |
|---|---|
| `WAIT_SOURCES` | USB-C or the dock is still plugged in |
| `DRAIN_VBUS` | both are unplugged; VBUS is discharging |
| `PENDING` | ship mode accepted; the battery disconnects at the deadline (`off_in_ms`) |
| `FAILED` | the charger could not be reached over I2C |

Check on the bench (`docs/todo.md` §1): the dock or debug probe could keep VSYS or 3.3 V up,
and the QON wake time (`WKUP_DLY`) still needs choosing.

---

## 4. Development aids

- **Simulator** (`build/host/ui_sim.elf`) runs the real UI code with a fake backend: animated
  values, two fake applets, and fault, dock and USB toggles. Every frame goes through the
  real driver into the emulator, and screenshots are saved as 4× PNG.
  - `ui_sim.elf <out_dir>` walks through every screen.
  - `ui_sim.elf <out_dir> <script>` runs a script; the commands are in the file header
    (`wait`, `click`, `hold`, `turn`, `shot`, `set`).
- **Unit tests:** `build/host/test_ui.elf` (format, UTF-8, keys, encoder, canvas, row
  tracking, parameter checks, menu editing, display power) and the driver's `test_ssd1327`.
- **On the device:** Settings → Input test (keys, encoder, raw port) and Settings → Display
  test:
  - gray ramp;
  - orientation (corners and arrow), plus bright/dark pixel pairs that show a nibble swap;
  - checkerboard;
  - full white (current check);
  - fonts.

## 5. Bring-up checklist

1. CubeMX (also in the hardware TODO):
   - SPI3: transmit-only master, /32, mode 0, NSS off, TX DMA, SPI3 interrupt.
   - PA15: GPIO out, high.
   - PD2: GPIO out, low.
   - PC5: EXTI falling, EXTI9_5 interrupt.
   - TIM20: TI12 encoder mode with a filter.
   - It works without the DMA (blocking) and without the EXTI (polling), just slower.
2. Fit the PCF8574 address straps and set `FRONT_PANEL_KEYPAD_I2C_ADDRESS` to match (default
   0x20).
3. Display test → orientation pattern:
   - upside down: set `display_config()` to `Rotated180`;
   - pixel pairs swapped: `swap_nibbles = true`.
4. Input test: check every key, the encoder direction and counts per detent.
5. Fix R12 (VPP), then measure the panel current on the full-white pattern.

## 6. Decisions

- SHUTDOWN key: click = soft stop, hold = power off through the BQ25792 ship mode (ship
  FET Q3). The power off waits for every power source to be unplugged (§3.7).
- Applet start: hold-to-start (1 s) for applets with `needs_arming()`; stopping is one
  press.
- Panel vs host: equal peers, last writer wins, live update. An edit of a value the host
  changes is cancelled with "Changed by host".
- Graphics: own small canvas and fonts, native 4 bpp, static memory.
- Orientation: the FPC tail is at the bottom of the IO board, the module's natural
  orientation (`Orientation::Normal`, re-map 0x51; check on hardware).
- Encoder clockwise = focus down / value up; a setting reverses it.
- HRTIM_EEV9 (SW4): reserved for a later HRTIM feature (delayed turn-off of the resonant
  stage at current zero cross). The panel ignores it.
- PCF8574 address: a compile-time constant.

## 7. Not done yet

The full list with work items is in `docs/todo.md`.

- Settings and presets are in RAM only. There is no AT24CS32 driver yet; `kParamPersist`
  and `save_config()` are ready for it.
- Not built yet:
  - the scope screen (arm/force/stop);
  - the log tail;
  - the "about" text per applet;
  - the protocol `GetScreen` screenshot.
- RAM: bss is now about 100 KB of 128 KB. Raising the FreeRTOS heap to 20 KB (hardware TODO)
  leaves little headroom. The 32 KB oscilloscope rings are the largest single user.
