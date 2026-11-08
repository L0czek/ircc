#include "power_path.hpp"

#if defined(resonant_converter_controller)

#include "FreeRTOS.h"
#include "board.h"
#include "board_timer.hpp"
#include "log.hpp"
#include "task.h"
#include "usb_pd_policy.hpp"
#include <algorithm>

namespace {

constexpr uint32_t kPeriodMs = 20;
constexpr uint32_t kFullPollEvery = 10;          // full poll (mux, battery, OTG faults, VOUT) every 200 ms
constexpr uint32_t kWakeFlag = 0x1u;             // set by UsbPdPort::wake() and PowerPath::wake()
constexpr uint32_t kVbusPresentMinMv = 3500;     // AC1_PRESENT: VAC1 > 3.4 V typ (faster than the ADC)
constexpr uint32_t kNoAttachInfoMs = 500;        // VAC1 present without a Type-C attach: assume default USB
constexpr uint32_t kTypeCVindpmMv = 4250;        // 5 V input without a contract
constexpr uint32_t kDockSwitchMinVbatMv = 12500; // the mux swap pauses the converter: the battery carries VSYS
constexpr uint32_t kSourceStopMarginMv = 600;    // stop sourcing below source_min - margin
constexpr uint32_t kDischargeDoneMv = 700;       // vSafe0V with margin
constexpr uint32_t kDischargeMaxMs = 500;
constexpr uint32_t kBridgeCheckMs = 100;         // U26 soft start (DVDT 3.3 nF) takes a few ms
constexpr uint32_t kBridgeVbusMinMv = 4500;      // VBUS through the bridge must reach this
constexpr uint32_t kShipDelayMs = 10000;         // SDRV_DLY=0: the chip waits 10 s after the write
constexpr uint32_t kShipRetryMs = 500;           // ship mode refused (VBUS still high): try again
constexpr uint8_t kOffUsbC = 0x1;                // off_inputs_ bits
constexpr uint8_t kOffDock = 0x2;
constexpr uint8_t kOffVbus = 0x4;
constexpr uint8_t kOffBattery = 0x8;
constexpr float kOtgVoltageV = 5.0f;
constexpr float kOtgCurrentA = 3.2f;             // above the advertised 3 A (IOTG accuracy +-2.2 %)

constexpr uint32_t kStackBytes = 1536;
alignas(8) uint64_t g_stack[kStackBytes / 8];
StaticTask_t g_tcb;

using Lock = board_timer::IrqLock;   // PRIMASK: atomic against vout_changing() on other threads

bool same_target(const PowerPathTarget& a, const PowerPathTarget& b) {
    return a.input == b.input && a.iindpm_a == b.iindpm_a && a.vindpm_v == b.vindpm_v && a.otg == b.otg &&
           a.otg_voltage_v == b.otg_voltage_v && a.otg_current_a == b.otg_current_a;
}

const char* input_name(ChargerInputSource s) {
    switch (s) {
        case ChargerInputSource_CHARGER_INPUT_AC1: return "USB-C";
        case ChargerInputSource_CHARGER_INPUT_AC2: return "dock";
        default: return "auto";
    }
}

const char* source_state_name(UsbPdSourceState s) {
    switch (s) {
        case UsbPdSourceState_USB_PD_SOURCE_READY:           return "ready";
        case UsbPdSourceState_USB_PD_SOURCE_ACTIVE:          return "active";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK:    return "blocked (input forced to USB-C)";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY: return "blocked (battery)";
        case UsbPdSourceState_USB_PD_SOURCE_BLOCKED_VOUT:    return "blocked (VOUT not 5 V / 3 A)";
        default:                                             return "disabled";
    }
}

bool vout_fits(const VoutState& v) {
    return usb_pd::vout_fits_source(v.known, v.en_pin, v.enabled, v.fault, v.mv, v.ilim_ma);
}

bool same_vout(const VoutState& a, const VoutState& b) {
    return a.known == b.known && a.en_pin == b.en_pin && a.enabled == b.enabled && a.mv == b.mv &&
           a.ilim_ma == b.ilim_ma;
}

} // namespace

void PowerPath::start() noexcept {
    if (thread_) {
        return;
    }
    vout_.set_listener(this);
    osThreadAttr_t attr{};
    attr.name = "usbpd-power";
    attr.attr_bits = osThreadDetached;
    attr.cb_mem = &g_tcb;
    attr.cb_size = sizeof(g_tcb);
    attr.stack_mem = g_stack;
    attr.stack_size = kStackBytes;
    attr.priority = osPriorityNormal1;   // above message-handler / power-service, below PE and CAD
    thread_ = osThreadNew(&PowerPath::thread_main, this, &attr);
    if (thread_) {
        port_.set_wake_thread(thread_);
    } else {
        os::error("PowerPath: thread creation failed\n");
    }
}

void PowerPath::thread_main(void* arg) {
    static_cast<PowerPath*>(arg)->run();
}

void PowerPath::run() noexcept {
    uint32_t cycle = 0;
    for (;;) {
        const uint32_t flags = osThreadFlagsWait(kWakeFlag, osFlagsWaitAny, kPeriodMs);
        const bool woken = (flags & 0x80000000u) == 0;   // not an osFlagsError* code (timeout)
        step(woken || (cycle++ % kFullPollEvery) == 0);
    }
}

void PowerPath::wake() noexcept {
    osThreadId_t id = thread_;
    if (id != nullptr) {
        (void)osThreadFlagsSet(id, kWakeFlag);
    }
}

// ---- Bridge (U26 EN, PC13) ----

bool PowerPath::bridge_pin_high() noexcept {
    return (g_board_usb_vout_bridge.GPIOx->ODR & g_board_usb_vout_bridge.pin) != 0;
}

void PowerPath::open_bridge() noexcept {
    HAL_GPIO_WritePin(g_board_usb_vout_bridge.GPIOx, g_board_usb_vout_bridge.pin, GPIO_PIN_RESET);
}

bool PowerPath::close_bridge() noexcept {
    // Atomic against vout_changing(): either it runs later and sees the bridge closed (and
    // opens it if needed), or it ran since poll_vout() and the bridge stays open.
    Lock lock;
    if (vout_seq_.load(std::memory_order_relaxed) != vout_seq_polled_) {
        return false;
    }
    HAL_GPIO_WritePin(g_board_usb_vout_bridge.GPIOx, g_board_usb_vout_bridge.pin, GPIO_PIN_SET);
    return true;
}

void PowerPath::vout_changing(const VoutState& next) noexcept {
    const bool fits = vout_fits(next);
    bool tripped = false;
    {
        Lock lock;
        vout_seq_.fetch_add(1, std::memory_order_relaxed);
        if (!fits && bridge_pin_high()) {
            open_bridge();
            tripped = true;
        }
    }
    if (tripped) {
        bridge_tripped_.store(true, std::memory_order_relaxed);
    }
    wake();   // re-read VOUT now
}

void PowerPath::poll_vout() noexcept {
    vout_seq_polled_ = vout_seq_.load(std::memory_order_relaxed);
    const auto polled = vout_.poll_vout();
    VoutState v{};
    if (polled) {
        v = *polled;
    }
    if (!same_vout(v, vout_state_)) {
        vout_failed_ = false;   // a new VOUT setting gets a new try
    }
    if (v.fault && bridge_closed_ms_) {
        os::warn("USB-PD: VOUT fault while bridged to USB-C\n");
    }
    vout_state_ = v;
    vout_fits_ = vout_fits(v);
}

// ---- Source ----

PowerPath::Path PowerPath::update_source(const ChargerInputPoll& poll, const UsbPdPort::Config& cfg,
                                         bool sourcing) noexcept {
    const uint32_t min_mv =
        active_ == Path::Otg ? cfg.source_min_vbat_mv - kSourceStopMarginMv : cfg.source_min_vbat_mv;
    UsbPdSourceState state = UsbPdSourceState_USB_PD_SOURCE_READY;
    Path path = Path::None;
    if (!cfg.source_enabled) {
        state = UsbPdSourceState_USB_PD_SOURCE_DISABLED;
    } else if (poll.ac2_present) {
        // OTG cannot run (it needs VBUS below UVLO): VOUT through the bridge, as it is.
        if (poll.settings.source_mode == ChargerInputSource_CHARGER_INPUT_AC1) {
            state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK;   // VAC1 is an input: no bridge into it
        } else if (!vout_fits_ || vout_failed_) {
            state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_VOUT;
        } else {
            path = Path::Vout;
        }
    } else if (!battery_present_ || vbat_otg_low_ || otg_fault_ || poll.vbat_mv < min_mv) {
        state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY;
    } else {
        path = Path::Otg;
    }
    port_.set_source_allowed(path != Path::None);
    port_.set_dock_present(poll.ac2_present);

    const Path shown = sourcing && active_ != Path::None ? active_ : path;
    if (path != Path::None && sourcing && active_ == path) {
        state = UsbPdSourceState_USB_PD_SOURCE_ACTIVE;
    }
    const UsbPdSourcePath proto_path = shown == Path::Vout  ? UsbPdSourcePath_USB_PD_SOURCE_PATH_VOUT
                                       : shown == Path::Otg ? UsbPdSourcePath_USB_PD_SOURCE_PATH_OTG
                                                            : UsbPdSourcePath_USB_PD_SOURCE_PATH_NONE;
    port_.set_source_state(state, proto_path);
    if (state != logged_source_ || shown != logged_path_) {
        os::info("USB-PD: source %s%s (VBAT %lu mV, VOUT %lu mV %s)\n", source_state_name(state),
                 shown == Path::Vout ? " via VOUT" : shown == Path::Otg ? " via OTG" : "",
                 static_cast<unsigned long>(poll.vbat_mv), static_cast<unsigned long>(vout_state_.mv),
                 vout_state_.enabled && vout_state_.en_pin ? "on" : "off");
        logged_source_ = state;
        logged_path_ = shown;
    }
    return path;
}

void PowerPath::step(bool full) noexcept {
    ChargerInputPoll poll;
    const auto polled = charger_.poll_inputs(poll, full);
    if (!polled) {
        // Keep the last VBUS value: a bus glitch must not look like a detach. No new source.
        port_.set_source_allowed(false);
        return;
    }
    const uint32_t now = HAL_GetTick();

    if (full || vout_seq_.load(std::memory_order_relaxed) != vout_seq_polled_) {
        poll_vout();
    }
    if (bridge_tripped_.exchange(false, std::memory_order_relaxed)) {
        os::warn("USB-PD: VOUT change, bridge to USB-C opened\n");
    }

    // VBUS for the stack's vSafe0V / vSafe5V checks
    uint32_t vbus = poll.vac1_mv;
    if (poll.ac1_present && vbus < kVbusPresentMinMv) {
        vbus = kVbusPresentMinMv;
    }
    port_.set_vbus_mv(vbus);
    if (poll.ac1_present) {
        ac1_since_ms_ = ac1_since_ms_ ? ac1_since_ms_ : (now ? now : 1);
    } else {
        ac1_since_ms_ = 0;
    }

    step_power_off(poll, full, now);

    const UsbPdPort::Snapshot s = port_.take_snapshot();
    UsbPdPort::Config cfg = port_.config();
    if (off_state_.load(std::memory_order_relaxed) != PowerOffState_POWER_OFF_IDLE) {
        cfg.source_enabled = false;   // powering off: no USB-C source (ends a running one)
    }
    if (full) {
        battery_present_ = poll.battery_present;
        vbat_otg_low_ = poll.vbat_otg_low;
        en_acdrv2_ = poll.en_acdrv2;
        if (active_ == Path::Otg && poll.otg_fault) {
            otg_fault_ = true;
            os::warn("USB-PD: OTG fault (OVP %d UVP %d), source stopped\n", poll.otg_ovp ? 1 : 0, poll.otg_uvp ? 1 : 0);
        }
    }
    if (!s.attached) {
        otg_fault_ = false;
        recovery_requested_ = false;
    }
    if (!poll.ac2_present) {
        vout_failed_ = false;
    }

    // ---- Source path ----
    const bool sourcing = s.attached && s.role == USBPD_APP_ROLE_SOURCE;
    const Path wanted = update_source(poll, cfg, sourcing);
    const bool want_vbus = sourcing && s.source_vbus;
    const bool was_on = active_ != Path::None;
    if (want_vbus && active_ == Path::None && wanted != Path::None && !recovery_requested_) {
        active_ = wanted;   // fixed until VBUS goes off
    }
    if (active_ != Path::None && (!want_vbus || wanted != active_)) {
        active_ = Path::None;
    }
    if (sourcing && (wanted == Path::None || (want_vbus && active_ == Path::None)) && !recovery_requested_) {
        // Dock arrived or left, battery low, VOUT changed, OTG fault or disabled: end the
        // source role so the sink sees a detach; the next attach gets the current path.
        os::info("USB-PD: ending the source role\n");
        recovery_requested_ = true;
        usbpd_port_error_recovery();
    }

    // Bridge off before the charger target changes: the mux must never take VAC1 while
    // VOUT drives it.
    if (active_ != Path::Vout && (bridge_closed_ms_ || bridge_pin_high())) {
        open_bridge();
        bridge_closed_ms_ = 0;
    }
    if (bridge_closed_ms_ && !bridge_pin_high()) {
        bridge_closed_ms_ = 0;   // opened by vout_changing()
    }

    const bool otg = active_ == Path::Otg;
    if ((was_on && active_ == Path::None) || s.discharge) {
        discharge_until_ms_ = now + kDischargeMaxMs;
    }

    // VAC1 discharge after the source output stops; never while a partner supplies us.
    const bool discharge = discharge_until_ms_ != 0 && active_ == Path::None &&
                           !(s.attached && s.role == USBPD_APP_ROLE_SINK) &&
                           static_cast<int32_t>(discharge_until_ms_ - now) > 0 && poll.vac1_mv > kDischargeDoneMv;
    if (!discharge) {
        discharge_until_ms_ = 0;
    }
    if (discharge != discharge_on_) {
        const auto set = charger_.set_vac1_discharge(discharge);
        if (set) {
            discharge_on_ = discharge;
        }
    }

    // ---- Input selection ----
    const ChargerInputSettings& in = poll.settings;
    ChargerInputSource input = ChargerInputSource_CHARGER_INPUT_AUTO;
    if (in.source_mode == ChargerInputSource_CHARGER_INPUT_AC1 || in.source_mode == ChargerInputSource_CHARGER_INPUT_AC2) {
        input = in.source_mode;
    } else if (poll.ac2_present && (sourcing || wanted == Path::Vout)) {
        // Sourcing (or ready to): USB-C is no input, and AC2 keeps EN_ACDRV1=0 for the bridge.
        input = ChargerInputSource_CHARGER_INPUT_AC2;
        dock_warned_ = false;
    } else if (poll.ac2_present) {
        const bool on_dock = en_acdrv2_ || !poll.ac1_present;
        const bool can_switch = battery_present_ && poll.vbat_mv >= kDockSwitchMinVbatMv;
        if (on_dock || can_switch) {
            input = ChargerInputSource_CHARGER_INPUT_AC2;
            dock_warned_ = false;
        } else {
            input = ChargerInputSource_CHARGER_INPUT_AC1;
            if (!dock_warned_) {
                os::warn("USB-PD: dock present but the battery cannot carry the switch (VBAT %lu mV), staying on USB-C\n",
                         static_cast<unsigned long>(poll.vbat_mv));
                dock_warned_ = true;
            }
        }
    } else if (poll.ac1_present && !sourcing) {
        input = ChargerInputSource_CHARGER_INPUT_AC1;
    }
    if (!poll.ac2_present) {
        dock_vindpm_mv_ = 0;
    } else if (dock_vindpm_mv_ == 0) {
        dock_vindpm_mv_ = usb_pd::vindpm_for_mv(poll.vac2_mv);   // measured when the dock appeared
    }

    // ---- Input limits ----
    bool have_limits = true;
    uint32_t usb_ma = 0;
    uint32_t usb_vindpm = kTypeCVindpmMv;
    if (s.attached && s.role == USBPD_APP_ROLE_SINK) {
        if (s.contract) {
            usb_ma = s.contract_ma;
            usb_vindpm = usb_pd::vindpm_for_mv(s.contract_mv);
        } else {
            usb_ma = usb_pd::typec_current_ma(static_cast<uint8_t>(s.rp));
        }
        if (s.request_in_flight) {
            usb_ma = std::min(usb_ma, s.request_ma);
            usb_vindpm = std::min(usb_vindpm, usb_pd::vindpm_for_mv(s.request_mv));
        }
    } else if (!sourcing && ac1_since_ms_ && now - ac1_since_ms_ > kNoAttachInfoMs) {
        usb_ma = usb_pd::typec_current_ma(USBPD_APP_RP_DEFAULT);   // VBUS without a Type-C attach
    } else if (input != ChargerInputSource_CHARGER_INPUT_AC2) {
        have_limits = false;   // boot / just plugged: keep what the chip has until the CAD reports
    }
    port_.set_usb_limit_ma(usb_ma);

    PowerPathTarget t = last_;
    t.input = input;
    t.otg = otg;
    t.otg_voltage_v = kOtgVoltageV;
    t.otg_current_a = kOtgCurrentA;
    if (have_limits && !otg) {
        uint32_t ma = usb_ma;
        uint32_t vindpm = usb_vindpm;
        if (input == ChargerInputSource_CHARGER_INPUT_AC2) {
            ma = static_cast<uint32_t>(in.dock_limit_a * 1000.0f + 0.5f);
            vindpm = dock_vindpm_mv_;
        }
        ma = std::min(ma, static_cast<uint32_t>(in.current_cap_a * 1000.0f + 0.5f));
        vindpm = std::max(vindpm, static_cast<uint32_t>(in.voltage_floor_v * 1000.0f + 0.5f));
        t.iindpm_a = static_cast<float>(ma) / 1000.0f;
        t.vindpm_v = static_cast<float>(vindpm) / 1000.0f;
    }

    if ((have_limits || otg || have_target_) &&
        (!have_target_ || apply_failed_ || !same_target(t, last_))) {
        apply_failed_ = !charger_.apply_power_path(t);
        last_ = t;
        have_target_ = true;
    }

    // ---- Bridge on: only once the charger holds EN_ACDRV1=0 (dock selected, no OTG) ----
    if (active_ == Path::Vout) {
        const bool mux_ready = have_target_ && !apply_failed_ && !last_.otg &&
                               last_.input == ChargerInputSource_CHARGER_INPUT_AC2;
        if (!bridge_closed_ms_ && mux_ready && close_bridge()) {
            bridge_closed_ms_ = now ? now : 1;
            os::info("USB-PD: VOUT bridged to USB-C\n");
        } else if (bridge_closed_ms_ && now - bridge_closed_ms_ > kBridgeCheckMs && poll.vac1_mv < kBridgeVbusMinMv) {
            open_bridge();
            bridge_closed_ms_ = 0;
            vout_failed_ = true;   // until VOUT changes or the dock leaves
            os::warn("USB-PD: VBUS %lu mV through the bridge, VOUT not usable\n",
                     static_cast<unsigned long>(poll.vac1_mv));
        }
    }

    // ---- Log changes ----
    if (s.contract != logged_contract_ || s.contract_mv != logged_contract_mv_ || s.contract_ma != logged_contract_ma_) {
        if (s.contract) {
            os::info("USB-PD: %s contract %lu mV %lu mA\n", s.role == USBPD_APP_ROLE_SOURCE ? "source" : "sink",
                     static_cast<unsigned long>(s.contract_mv), static_cast<unsigned long>(s.contract_ma));
        } else if (logged_contract_) {
            os::info("USB-PD: no contract\n");
        }
        logged_contract_ = s.contract;
        logged_contract_mv_ = s.contract_mv;
        logged_contract_ma_ = s.contract_ma;
    }
    if (input != logged_input_) {
        os::info("USB-PD: input %s\n", input_name(input));
        logged_input_ = input;
    }
}

// ---- Power off ----

void PowerPath::request_ship_mode() noexcept {
    // Report it at once (the thread confirms it on its next step)
    PowerOffState expected = PowerOffState_POWER_OFF_IDLE;
    if (!off_state_.compare_exchange_strong(expected, PowerOffState_POWER_OFF_WAIT_SOURCES)) {
        expected = PowerOffState_POWER_OFF_FAILED;
        (void)off_state_.compare_exchange_strong(expected, PowerOffState_POWER_OFF_WAIT_SOURCES);
    }
    off_request_.store(OffRequest::Start, std::memory_order_relaxed);
    wake();
}

void PowerPath::cancel_power_off() noexcept {
    off_request_.store(OffRequest::Cancel, std::memory_order_relaxed);
    wake();
}

bool PowerPath::power_off_status(PowerOffStatus& out) const noexcept {
    const PowerOffState state = off_state_.load(std::memory_order_relaxed);
    if (state == PowerOffState_POWER_OFF_IDLE) {
        return false;
    }
    const uint8_t inputs = off_inputs_.load(std::memory_order_relaxed);
    out = PowerOffStatus{};
    out.state = state;
    out.usb_c_present = (inputs & kOffUsbC) != 0;
    out.dock_present = (inputs & kOffDock) != 0;
    out.vbus_present = (inputs & kOffVbus) != 0;
    out.battery_present = (inputs & kOffBattery) != 0;
    if (state == PowerOffState_POWER_OFF_PENDING) {
        const int32_t left = static_cast<int32_t>(off_deadline_ms_.load(std::memory_order_relaxed) - HAL_GetTick());
        out.has_off_in_ms = true;
        out.off_in_ms = left > 0 ? static_cast<uint32_t>(left) : 0;
    }
    return true;
}

void PowerPath::set_off_state(PowerOffState state) noexcept {
    static constexpr const char* kNames[] = {"idle", "waiting for the power sources to be unplugged",
                                             "draining VBUS", "ship mode in 10 s", "charger error"};
    if (logged_off_state_ != state) {
        os::info("Power off: %s\n", kNames[static_cast<size_t>(state) % 5]);
        logged_off_state_ = state;
    }
    off_state_.store(state, std::memory_order_relaxed);
}

void PowerPath::step_power_off(const ChargerInputPoll& poll, bool full, uint32_t now) noexcept {
    const OffRequest request = off_request_.exchange(OffRequest::None, std::memory_order_relaxed);
    PowerOffState state = off_state_.load(std::memory_order_relaxed);

    if (request == OffRequest::Cancel && state != PowerOffState_POWER_OFF_IDLE) {
        if (state == PowerOffState_POWER_OFF_PENDING) {
            const auto cancelled = charger_.cancel_ship_mode();
            if (!cancelled) {
                os::error("Power off: could not cancel ship mode\n");
            }
        }
        state = PowerOffState_POWER_OFF_IDLE;
    } else if (request == OffRequest::Start &&
               (state == PowerOffState_POWER_OFF_IDLE || state == PowerOffState_POWER_OFF_FAILED)) {
        state = PowerOffState_POWER_OFF_WAIT_SOURCES;
    }

    uint8_t inputs = 0;
    inputs |= poll.ac1_present ? kOffUsbC : 0;
    inputs |= poll.ac2_present ? kOffDock : 0;
    inputs |= poll.vbus_present ? kOffVbus : 0;
    inputs |= battery_present_ ? kOffBattery : 0;
    off_inputs_.store(inputs, std::memory_order_relaxed);

    const bool sources = poll.ac1_present || poll.ac2_present;
    switch (state) {
        case PowerOffState_POWER_OFF_WAIT_SOURCES:
            if (!sources) {
                state = PowerOffState_POWER_OFF_DRAIN_VBUS;
                next_ship_try_ms_ = now;
            }
            break;
        case PowerOffState_POWER_OFF_DRAIN_VBUS:
            if (sources) {
                state = PowerOffState_POWER_OFF_WAIT_SOURCES;   // plugged in again
            } else if (!poll.vbus_present && static_cast<int32_t>(now - next_ship_try_ms_) >= 0) {
                // The chip refuses (puts SDRV_CTRL back to 00) while it still sees VBUS.
                const auto accepted = charger_.enter_ship_mode();
                if (!accepted) {
                    state = PowerOffState_POWER_OFF_FAILED;
                } else if (*accepted) {
                    off_deadline_ms_.store(now + kShipDelayMs, std::memory_order_relaxed);
                    state = PowerOffState_POWER_OFF_PENDING;
                } else {
                    next_ship_try_ms_ = now + kShipRetryMs;
                }
            }
            break;
        case PowerOffState_POWER_OFF_PENDING:
            if (full) {
                // An adapter plugged in during the delay makes the chip return to IDLE.
                const auto pending = charger_.ship_mode_pending();
                if (pending && !*pending) {
                    os::warn("Power off: the charger left ship mode (power plugged in?)\n");
                    state = PowerOffState_POWER_OFF_WAIT_SOURCES;
                }
            }
            break;
        default:
            break;
    }

    // VBUS pull-down only while draining or waiting for the cut-off
    const bool discharge = state == PowerOffState_POWER_OFF_DRAIN_VBUS || state == PowerOffState_POWER_OFF_PENDING;
    if (discharge != vbus_discharge_on_) {
        const auto set = charger_.set_vbus_discharge(discharge);
        if (set) {
            vbus_discharge_on_ = discharge;
        }
    }
    set_off_state(state);
}

#else

void PowerPath::start() noexcept {}
void PowerPath::vout_changing(const VoutState&) noexcept {}
void PowerPath::request_ship_mode() noexcept {}
void PowerPath::cancel_power_off() noexcept {}
bool PowerPath::power_off_status(PowerOffStatus&) const noexcept { return false; }

#endif
