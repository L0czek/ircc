# USB-C / USB Power Delivery

The USB-C port (J8) is a dual-role power port driven by UCPD1 and the ST USB-PD stack
(`USBPDCORE_PD3_FULL`, DRP build). Power flows through the BQ25792 charger, except the
VOUT bridge:

| Path | Hardware | Firmware |
|---|---|---|
| USB-C sink | VBUS_USBC → BQ25792 **VAC1**, FET pair Q5 (ACDRV1) | input current from the PD contract or the Type-C Rp current |
| Dock DC | VBUS_DOCK → BQ25792 **VAC2**, FET pair Q4 (ACDRV2) | preferred input, limit `dock_input_current_limit` (default 3.0 A) |
| USB-C source, no dock | BQ25792 **OTG** from the battery onto VAC1 | 5 V 3 A, only with a good battery |
| USB-C source, dock | **VOUT** (TPS55289 U2, from VSYS) → **U26** TPS259474A eFuse (EN = PC13 `BRIDGE_VOUT_VBUS_USB`, 10 k pull-down, ILIM ≈ 3 A via R79 1.11 k, reverse blocking) → VBUS_USBC | 5 V 3 A, only while VOUT already is 5 V, enabled, ≥ 3 A, fault free |

There is no MCU ADC on VBUS: the stack's VBUS readings come from the BQ25792 VAC1 ADC and
AC1_PRESENT status, polled by the `usbpd-power` thread.

## Behaviour

* **Sink by default.** Fixed PDOs from 5 V to `max_sink_voltage` (20 V) are considered and
  the one with the highest power `V × min(Imax, max_sink_current)` is requested (ties: the
  higher voltage, less cable loss). The requested current is `min(Imax, max_sink_current)`
  (3 A default). PPS/APDOs are ignored for now.
* **The contract becomes the charger's input limit.** After PS_RDY the BQ25792 gets
  `IINDPM = contract current` and `VINDPM = 0.85 × contract voltage` (min 3.6 V). Before a
  request for a *lower* voltage or current is sent, IINDPM/VINDPM are first lowered to
  min(old, new), so the input never regulates above the new voltage.
  Without a PD contract (Type-C only source) the Rp level sets the limit: default USB
  0.5 A, 1.5 A, 3.0 A, with VINDPM 4.25 V.
* **Dock priority.** When the dock (AC2_PRESENT) is valid it is selected (EN_ACDRV2=1,
  EN_ACDRV1=0 in one write) and IINDPM becomes the dock limit. The swap pauses the
  converter, so it is only done when the battery can carry the system (battery present and
  VBAT ≥ 12.5 V); otherwise USB-C stays the input and a warning is logged. When the dock
  goes away the BQ25792 falls back to VAC1 on its own; the policy then restores the USB-C
  limit. The USB-C contract stays up while the dock supplies.
* **Source, one fixed PDO 5 V / 3 A (Rp 3.0 A)** when `source_enabled`, on one of two paths:
  * **No dock: OTG.** Battery present, VBAT ≥ `source_min_battery_voltage` (13.2 V default,
    4S pack; stop below that minus 0.6 V), no OTG fault. VBUS on = VOTG 5.00 V, IOTG 3.2 A,
    EN_OTG=1, EN_ACDRV1=1 (EN_ACDRV2=0).
  * **Dock: VOUT bridge.** OTG cannot run with the dock (it needs VBUS below UVLO). VOUT is
    used **as it is**: TPS55289 EN pin (PC15) high, output enabled, programmed 4.75–5.25 V,
    current limit ≥ 2.95 A and no SCP/OCP/OVP since the last poll (`usb_pd::vout_fits_source`).
    The firmware never changes VOUT for USB; with VOUT elsewhere the state is
    `BLOCKED_VOUT`. A host-forced USB-C input (`input_source` AC1) blocks it
    (`BLOCKED_DOCK`): VAC1 must not be an input while VOUT drives it. VBUS on = charger
    input forced to the dock (EN_ACDRV2=1, EN_ACDRV1=0 written and confirmed), then PC13
    high. VAC1 must reach 4.5 V within 100 ms, else the bridge opens and VOUT is marked
    unusable until its setting changes or the dock leaves.
  * **VOUT changes** go through `OutputVoltageRegulator`, which calls
    `PowerPath::vout_changing()` with the new state *before* the I2C write (bus lock held).
    A state that no longer fits opens the bridge at once (PRIMASK section, sequence counter
    against a concurrent close). TPS55289 faults are seen by the 200 ms poll; U26 limits
    the current meanwhile.
  * The path is fixed when VBUS turns on. If the wanted path changes (dock plugged in or
    out, VOUT changed, battery low, OTG fault) the port enters Type-C error recovery so the
    sink sees a detach; the next attach gets the new path. VBUS off = EN_OTG=0 / PC13 low and
    a short VAC1 pull-down (60 Ω) until VAC1 < 0.7 V.
* **Role swaps.** PR_Swap, DR_Swap and VCONN_Swap are rejected; VDMs are off (planned:
  vendor messages between our own boards and a debug-accessory mode).
* **Dead battery.** The external 5.1 kΩ Rd (R72/R73 through U12/U25) stay connected while
  PD0 (USB_DEAD_BATT_DISABLE) is low, which it is from reset until firmware runs. UCPD's own
  Rd is asserted in `CAD_Init` (inside `MX_USBPD_Init`); `USBPD_DPM_UserInit`, which runs
  right after it, drives PD0 high. The CC line therefore always sees an Rd and a source that
  powers a flat board keeps VBUS on.

## Input current policy (one owner)

The BQ25792 has a single IINDPM and VINDPM for both inputs, and the chip itself rewrites
them (ILIM_HIZ clamp, D+/D- detection, VINDPM re-measure at plug-in). Therefore:

* `Charger` owns the registers and re-applies everything after a watchdog expiry / chip
  reset (WD_STAT=1). It also sets, at init and after every expiry:
  `EN_EXTILIM=0` (the 127k/100k ILIM_HIZ divider clamps at ~1.4 A otherwise),
  `AUTO_INDET_EN=0` (D+/D- are not connected), `HVDCP_EN=0`, `EN_IBAT=1` (discharge
  current sensing), ADC D+/D- channels off (faster VAC1 refresh).
  Before clearing EN_EXTILIM, IINDPM is written to the limit currently in use (REG19
  ICO_ILIM), so taking the clamp away never raises the input current by itself.
* `PowerPath` (thread `usbpd-power`, 20 ms) decides: active input, IINDPM, VINDPM and OTG.
* Host settings (`ChargerConfigRequest`) are inputs to that decision:
  `input_current_limit` = cap on any input (3.3 = none), `input_voltage_limit` = VINDPM
  floor (3.6 = none), `input_source` AUTO = dock first / AC1 / AC2 = forced,
  `dock_input_current_limit`. `ChargerConfig.input_*_limit` report the values in use.

## Source gating

The CAD runs the DRP state machine (toggling Rd/Rp). A source attach completes only after
the CAD sees VBUS below vSafe0V (0.75 V). While `usbpd_app_source_allowed()` is false the
C side reports at least 1.0 V to the CAD **in the source role while not connected**, which
holds it in AttachWait.SRC: a sink sees Rp but never VBUS. If that hold lasts 1 s the C side
calls error recovery so that toggling resumes (a dual-role partner can then attach us as
its sink). A sink attach (partner Rp) is unaffected.

## Threads and contexts

| Context | Runs | May |
|---|---|---|
| CAD thread (Realtime) | VBUS getters, attach/detach | read cached state, set flags |
| PE thread (AboveNormal, 1400 B, static memory) | caps, request build/evaluate, hard reset | same; no I2C, no logging |
| UCPD ISR | hard-reset-received notification | ISR-safe only (critical section + osThreadFlagsSet) |
| `usbpd-power` (Normal1, 1536 B, static) | BQ25792 poll + policy, every 20 ms or on wake | I2C via `i2c_internal_bus_transaction`, logging |

`USBPD_DPM_TimerCounter()` is called every 1 ms from the TIM7 HAL tick
(main.c USER CODE Callback 1); the PE/PRL timers do not run otherwise.

## Interfaces

* `headers/usbpd_app.h` — C bridge between the stack USER CODE and the controller.
* `controller/inc/usbpd_app.hpp` — `UsbPdPort` (state, policy hooks, status/config for the
  host), `controller/inc/power_path.hpp` — `PowerPath` thread,
  `controller/inc/usb_pd_policy.hpp` — pure PDO/RDO helpers (host unit test).
* `Charger` additions used by PowerPath:

```cpp
struct ChargerInputPoll {             // Charger::poll_inputs, one bus transaction
    bool vbus_present, ac1_present, ac2_present, power_good;
    uint32_t vac1_mv, vac2_mv, vbat_mv;
    bool full;                        // fields below valid (read when requested)
    bool battery_present, otg_enabled, en_acdrv1, en_acdrv2, dis_acdrv;
    bool vbat_otg_low, ts_cold, ts_hot, otg_fault;   // REG1F / REG21
    bool watchdog_reapplied;          // WD_STAT was 1: settings were re-applied in this call
    ChargerInputSettings settings;    // host input settings (no I2C)
};
struct ChargerInputSettings { ChargerInputSource source_mode; float dock_limit_a;
                              float current_cap_a; float voltage_floor_v; };
struct PowerPathTarget {              // Charger::apply_power_path (writes only changes)
    ChargerInputSource input;         // AC1 / AC2 (one EN_ACDRV write) or AUTO (leave)
    float iindpm_a, vindpm_v;         // final values (cap/floor already applied)
    bool otg; float otg_voltage_v, otg_current_a;
};
std::expected<void, ControllerError> poll_inputs(ChargerInputPoll&, bool full) noexcept;
std::expected<void, ControllerError> apply_power_path(const PowerPathTarget&) noexcept;
std::expected<void, ControllerError> set_vac1_discharge(bool on) noexcept;
```

## Protocol

`SystemStatus.usb_pd` (UsbPdStatus) and `SystemStatus.usb_pd_config` (UsbPdConfig);
requests `set_usb_pd_config` (20) and `usb_pd_renegotiate` (21), both answered with
`system_status` + `result`; `ChargerConfigRequest.dock_input_current_limit`; error codes
`USBPD_*` (190-195). `UsbPdStatus.source_path` (NONE / OTG / VOUT) is the path in use
while ACTIVE, or the one a sink would get while READY. Raw PDOs are sent as `uint32` and decoded by hostctl. Settings live in
RAM (no persistent storage yet; U16 AT24CS32 on the internal bus is a candidate).

hostctl: `usb` (alias `pd`) — `usb source on|off`, `usb set vmax=<V> imax=<A> vbatmin=<V>`,
`usb renegotiate [hard]`, `usb edit`; `charger set idock=<A>`; USB-C panel in F2 Power and
a summary line in the F1 charger panel.

## Required CubeMX changes (not done in code)

1. **FreeRTOS heap** `configTOTAL_HEAP_SIZE` ≥ 20480 (it is 3072; controller threads fail
   to start, see docs/cubemx_oscilloscope_requirements.md). The PE thread gets static
   memory from `USBPD_DPM_UserInit`, so a USB-C attach no longer depends on the heap.
2. **UCPD1 interrupt priority.** `usbpd_devices_conf.h` enables UCPD1 at NVIC priority 4,
   above `configMAX_SYSCALL_INTERRUPT_PRIORITY` (5); the ISR calls FreeRTOS. Set the USBPD
   interrupt priority to 5 or lower urgency in CubeMX. Until then the UCPD1_IRQn USER CODE
   in stm32g4xx_it.c restores priority 5 at ISR entry.
3. **USBPD settings** (optional, the code overrides them in `usbpd_port_early_init`):
   DRP role toggle on, SNK/SRC toggle 40 ms, Rp 3.0 A, PR_Swap off, sink PDOs 5/9/15/20 V
   3 A, source PDO 5 V 3 A. Leaving the generated `CAD_RoleToggle = FALSE` with only `_DRP`
   defined would leave the CAD state machine pointer NULL.
4. **PD0 label**: name it `USB_DEAD_BATT_DISABLE` (main.c calls it `g_board_lcd_bl`; an alias
   `g_board_usb_dead_batt_disable` is defined in USER CODE PV).
5. **PC13 label** (optional): `BRIDGE_VOUT_VBUS_USB`, GPIO output, low at reset (already
   GPIO_Output in the .ioc; the alias `g_board_usb_vout_bridge` is in USER CODE PV).

## Verify on hardware

* U12/U25 (TPFSA515UCX) polarity: CC-to-GND ≈ 5.1 kΩ with PD0 low, open with PD0 high.
* Both ACFET pairs fitted: ACRB1_STAT = ACRB2_STAT = 1 after POR.
* VAC1 refresh period in continuous ADC mode (attach/detach latency).
* OTG start with the dock absent; dock arrival while sourcing ends the source role.
* **VOUT_ENABLE (PC15) is never driven by the firmware** (R6 10 k pull-down): the TPS55289
  stays in shutdown and the VOUT bridge stays `BLOCKED_VOUT`. Decide who drives it
  (OutputVoltageRegulator::set_enabled is the obvious owner), and check whether the
  TPS55289 I2C answers while EN is low.
* VOUT bridge with the dock: VAC1 ≈ VOUT, ACRB1/ACDRV1 off (no current back into the
  charger), U26 PG/ILIM behaviour at a 3 A sink.
* Dock unplugged while bridging: the BQ25792 must not turn on ACDRV1 in the ≤ 20 ms before
  PowerPath opens the bridge (EN_ACDRV2 was host-written; check it does not fail over).
