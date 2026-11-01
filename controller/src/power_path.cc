#include "power_path.hpp"

#if defined(resonant_converter_controller)

#include "FreeRTOS.h"
#include "log.hpp"
#include "task.h"
#include "usb_pd_policy.hpp"
#include <algorithm>

namespace {

constexpr uint32_t kPeriodMs = 20;
constexpr uint32_t kFullPollEvery = 10;          // full poll (mux, battery, OTG faults) every 200 ms
constexpr uint32_t kWakeFlag = 0x1u;             // set by UsbPdPort::wake()
constexpr uint32_t kVbusPresentMinMv = 3500;     // AC1_PRESENT: VAC1 > 3.4 V typ (faster than the ADC)
constexpr uint32_t kNoAttachInfoMs = 500;        // VAC1 present without a Type-C attach: assume default USB
constexpr uint32_t kTypeCVindpmMv = 4250;        // 5 V input without a contract
constexpr uint32_t kDockSwitchMinVbatMv = 12500; // the mux swap pauses the converter: the battery carries VSYS
constexpr uint32_t kSourceStopMarginMv = 600;    // stop sourcing below source_min - margin
constexpr uint32_t kDischargeDoneMv = 700;       // vSafe0V with margin
constexpr uint32_t kDischargeMaxMs = 500;
constexpr float kOtgVoltageV = 5.0f;
constexpr float kOtgCurrentA = 3.2f;             // above the advertised 3 A (IOTG accuracy +-2.2 %)

constexpr uint32_t kStackBytes = 1536;
alignas(8) uint64_t g_stack[kStackBytes / 8];
StaticTask_t g_tcb;

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

} // namespace

void PowerPath::start() noexcept {
    if (thread_) {
        return;
    }
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

bool PowerPath::update_source_allowed(const ChargerInputPoll& poll, const UsbPdPort::Config& cfg) noexcept {
    const uint32_t min_mv = source_allowed_ ? cfg.source_min_vbat_mv - kSourceStopMarginMv : cfg.source_min_vbat_mv;
    UsbPdSourceState state = UsbPdSourceState_USB_PD_SOURCE_READY;
    if (!cfg.source_enabled) {
        state = UsbPdSourceState_USB_PD_SOURCE_DISABLED;
    } else if (poll.ac2_present) {
        state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK;   // OTG needs VBUS below UVLO
    } else if (!battery_present_ || vbat_otg_low_ || otg_fault_ || poll.vbat_mv < min_mv) {
        state = UsbPdSourceState_USB_PD_SOURCE_BLOCKED_BATTERY;
    }
    source_allowed_ = state == UsbPdSourceState_USB_PD_SOURCE_READY;
    port_.set_source_allowed(source_allowed_);
    port_.set_dock_present(poll.ac2_present);
    if (source_allowed_ && otg_) {
        state = UsbPdSourceState_USB_PD_SOURCE_ACTIVE;
    }
    port_.set_source_state(state);
    if (state != logged_source_) {
        os::info("USB-PD: source %s (VBAT %lu mV)\n",
                 state == UsbPdSourceState_USB_PD_SOURCE_READY         ? "ready"
                 : state == UsbPdSourceState_USB_PD_SOURCE_ACTIVE      ? "active"
                 : state == UsbPdSourceState_USB_PD_SOURCE_BLOCKED_DOCK ? "blocked (dock)"
                 : state == UsbPdSourceState_USB_PD_SOURCE_DISABLED     ? "disabled"
                                                                        : "blocked (battery)",
                 static_cast<unsigned long>(poll.vbat_mv));
        logged_source_ = state;
    }
    return source_allowed_;
}

void PowerPath::step(bool full) noexcept {
    ChargerInputPoll poll;
    if (auto r = charger_.poll_inputs(poll, full); !r) {
        // Keep the last VBUS value: a bus glitch must not look like a detach. No new source.
        port_.set_source_allowed(false);
        source_allowed_ = false;
        return;
    }
    const uint32_t now = HAL_GetTick();

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

    const UsbPdPort::Snapshot s = port_.take_snapshot();
    const UsbPdPort::Config cfg = port_.config();
    if (full) {
        battery_present_ = poll.battery_present;
        vbat_otg_low_ = poll.vbat_otg_low;
        en_acdrv2_ = poll.en_acdrv2;
        if (otg_ && poll.otg_fault) {
            otg_fault_ = true;
            os::warn("USB-PD: OTG fault (OVP %d UVP %d), source stopped\n", poll.otg_ovp ? 1 : 0, poll.otg_uvp ? 1 : 0);
        }
    }
    if (!s.attached) {
        otg_fault_ = false;
        recovery_requested_ = false;
    }

    // ---- Source (OTG) ----
    const bool allowed = update_source_allowed(poll, cfg);
    const bool sourcing = s.attached && s.role == USBPD_APP_ROLE_SOURCE;
    bool otg = sourcing && s.source_vbus && allowed;
    if (sourcing && !allowed && !recovery_requested_) {
        // Dock arrived, battery low, OTG fault or disabled: end the source role so the sink
        // sees a detach (VBUS goes off below as well).
        os::info("USB-PD: ending the source role\n");
        recovery_requested_ = true;
        usbpd_port_error_recovery();
    }
    if ((otg_ && !otg) || s.discharge) {
        discharge_until_ms_ = now + kDischargeMaxMs;
    }
    otg_ = otg;

    // VAC1 discharge after the source output stops; never while a partner supplies us.
    const bool discharge = discharge_until_ms_ != 0 && !otg && !(s.attached && s.role == USBPD_APP_ROLE_SINK) &&
                           static_cast<int32_t>(discharge_until_ms_ - now) > 0 && poll.vac1_mv > kDischargeDoneMv;
    if (!discharge) {
        discharge_until_ms_ = 0;
    }
    if (discharge != discharge_on_) {
        if (charger_.set_vac1_discharge(discharge)) {
            discharge_on_ = discharge;
        }
    }

    // ---- Input selection ----
    const ChargerInputSettings& in = poll.settings;
    ChargerInputSource input = ChargerInputSource_CHARGER_INPUT_AUTO;
    if (in.source_mode == ChargerInputSource_CHARGER_INPUT_AC1 || in.source_mode == ChargerInputSource_CHARGER_INPUT_AC2) {
        input = in.source_mode;
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
    } else if (poll.ac1_present) {
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
    } else if (ac1_since_ms_ && now - ac1_since_ms_ > kNoAttachInfoMs) {
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

#else

void PowerPath::start() noexcept {}

#endif
