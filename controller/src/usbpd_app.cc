#include "usbpd_app.hpp"

#if defined(resonant_converter_controller)

#include "board.h"
#include "board_timer.hpp"
#include "cmsis_os2.h"
#include "log.hpp"
#include "usb_pd_policy.hpp"
#include <algorithm>
#include <cstring>

namespace {

constinit UsbPdPort g_port;

// PowerPath waits on this thread flag (power_path.cc)
constexpr uint32_t kWakeFlag = 0x1u;

using Lock = board_timer::IrqLock;   // PRIMASK: usable from the UCPD ISR as well

bool in_range(float v, float lo, float hi) { return v >= lo && v <= hi; }   // false for NaN

} // namespace

UsbPdPort& usb_pd_port() noexcept {
    return g_port;
}

void UsbPdPort::wake() noexcept {
    if (void* id = wake_thread_.load(); id != nullptr) {
        (void)osThreadFlagsSet(static_cast<osThreadId_t>(id), kWakeFlag);
    }
}

UsbPdPort::Snapshot UsbPdPort::take_snapshot() noexcept {
    Lock lock;
    Snapshot s = st_.s;
    st_.s.discharge = false;
    return s;
}

UsbPdPort::Config UsbPdPort::config() const noexcept {
    Lock lock;
    return cfg_;
}

// =============================================================================
// Stack callbacks (CAD / PE threads, UCPD ISR): state only, then wake PowerPath
// =============================================================================

bool UsbPdPort::attached(usbpd_app_role_t role, uint8_t cc_line, usbpd_app_rp_t rp) noexcept {
    if (role == USBPD_APP_ROLE_SOURCE && !source_allowed()) {
        return false;   // the C side enters error recovery
    }
    {
        Lock lock;
        const uint32_t hard_resets = st_.s.hard_resets;
        st_ = State{};
        st_.s.attached = true;
        st_.s.role = role;
        st_.s.cc_line = cc_line;
        st_.s.rp = role == USBPD_APP_ROLE_SINK ? rp : USBPD_APP_RP_NONE;
        st_.s.attach_ms = HAL_GetTick();
        st_.s.source_vbus = role == USBPD_APP_ROLE_SOURCE;
        st_.s.hard_resets = hard_resets;
    }
    wake();
    return true;
}

void UsbPdPort::detached() noexcept {
    {
        Lock lock;
        const uint32_t hard_resets = st_.s.hard_resets;
        st_ = State{};
        st_.s.hard_resets = hard_resets;
    }
    wake();
}

void UsbPdPort::source_caps(const uint32_t* pdos, uint32_t count) noexcept {
    Lock lock;
    st_.caps_count = std::min<uint32_t>(count, 7u);
    std::memcpy(st_.caps, pdos, st_.caps_count * sizeof(uint32_t));
    st_.pd_capable = true;
}

uint32_t UsbPdPort::build_request() noexcept {
    uint32_t rdo;
    {
        Lock lock;
        const usb_pd::Selection sel =
            usb_pd::select_sink_pdo(st_.caps, st_.caps_count, cfg_.max_sink_mv, cfg_.max_sink_ma);
        st_.selected = sel.position;
        // Until PS_RDY PowerPath keeps the input limits at min(current, requested), so a step
        // down (20 V -> 5 V, or less current) never leaves VINDPM/IINDPM above the new contract.
        st_.s.request_in_flight = true;
        st_.s.request_mv = sel.mv;
        st_.s.request_ma = sel.ma;
        rdo = usb_pd::fixed_rdo(sel.position, sel.ma, sel.ma);
    }
    wake();
    return rdo;
}

uint32_t UsbPdPort::sink_pdos(uint32_t* pdos, uint32_t max) const noexcept {
    const Config cfg = config();
    return usb_pd::sink_pdos(pdos, max, cfg.max_sink_mv, cfg.max_sink_ma,
                             dock_present_.load(std::memory_order_relaxed));
}

usbpd_app_request_result_t UsbPdPort::evaluate_request(uint32_t rdo) noexcept {
    if (!source_allowed() || usb_pd::evaluate_source_request(rdo) != usb_pd::RequestVerdict::Accept) {
        return USBPD_APP_REQ_REJECT;
    }
    Lock lock;
    st_.s.request_in_flight = true;
    st_.s.request_mv = usb_pd::kSourceMv;
    st_.s.request_ma = usb_pd::decode_rdo(rdo).op_ma;
    st_.selected = 1;
    return USBPD_APP_REQ_ACCEPT;
}

void UsbPdPort::event(usbpd_app_event_t event) noexcept {
    {
        Lock lock;
        Snapshot& s = st_.s;
        switch (event) {
            case USBPD_APP_EV_EXPLICIT_CONTRACT:
                if (s.request_in_flight) {
                    s.contract = true;
                    s.contract_mv = s.request_mv;
                    s.contract_ma = s.request_ma;
                }
                s.request_in_flight = false;
                s.hard_reset = false;
                break;
            case USBPD_APP_EV_REQUEST_REJECTED:
            case USBPD_APP_EV_REQUEST_WAIT:
                s.request_in_flight = false;   // the previous contract (if any) stays
                break;
            case USBPD_APP_EV_HARD_RESET:
                s.contract = false;
                s.request_in_flight = false;
                s.hard_reset = true;
                ++s.hard_resets;
                break;
            case USBPD_APP_EV_SNK_READY:
            case USBPD_APP_EV_SRC_READY:
                s.hard_reset = false;
                break;
            case USBPD_APP_EV_PE_DISABLED:
                st_.pd_capable = false;        // Type-C current only
                s.request_in_flight = false;
                s.hard_reset = false;
                break;
            case USBPD_APP_EV_REQUEST_ACCEPTED:
            default:
                break;
        }
    }
    wake();
}

void UsbPdPort::hard_reset(usbpd_app_role_t role, usbpd_app_hard_reset_t phase) noexcept {
    {
        Lock lock;
        Snapshot& s = st_.s;
        switch (phase) {
            case USBPD_APP_HR_START:
                s.hard_reset = true;
                s.contract = false;
                s.request_in_flight = false;
                if (role == USBPD_APP_ROLE_SOURCE) {
                    s.source_vbus = false;   // VBUS to vSafe0V, then back on after tSrcRecover
                    s.discharge = true;
                }
                break;
            case USBPD_APP_HR_WAIT_VSAFE5V:
                if (role == USBPD_APP_ROLE_SOURCE) {
                    s.source_vbus = true;    // PowerPath ends the source role if no longer allowed
                }
                break;
            case USBPD_APP_HR_COMPLETED:
            case USBPD_APP_HR_FAILED:
                s.hard_reset = false;
                break;
            case USBPD_APP_HR_WAIT_VSAFE0V:
            default:
                break;
        }
    }
    wake();
}

// =============================================================================
// Host
// =============================================================================

void UsbPdPort::fill_status(UsbPdStatus& status) noexcept {
    usbpd_port_info_t info{};
    usbpd_port_get_info(&info);
    State st;
    {
        Lock lock;
        st = st_;
    }
    const Snapshot& s = st.s;

    status = UsbPdStatus_init_zero;
    if (!s.attached) {
        status.state = UsbPdState_USB_PD_STATE_DETACHED;
    } else if (s.hard_reset) {
        status.state = UsbPdState_USB_PD_STATE_HARD_RESET;
    } else if (s.contract) {
        status.state = UsbPdState_USB_PD_STATE_CONTRACT;
    } else {
        status.state = UsbPdState_USB_PD_STATE_ATTACHED;
    }
    status.power_role = s.role == USBPD_APP_ROLE_SINK     ? UsbPdPowerRole_USB_PD_POWER_ROLE_SINK
                        : s.role == USBPD_APP_ROLE_SOURCE ? UsbPdPowerRole_USB_PD_POWER_ROLE_SOURCE
                                                          : UsbPdPowerRole_USB_PD_POWER_ROLE_NONE;
    if (s.attached) {
        status.has_data_role = true;
        status.data_role = info.data_role == 2   ? UsbPdDataRole_USB_PD_DATA_ROLE_DFP
                           : info.data_role == 1 ? UsbPdDataRole_USB_PD_DATA_ROLE_UFP
                                                 : UsbPdDataRole_USB_PD_DATA_ROLE_NONE;
        status.has_cc_line = true;
        status.cc_line = s.cc_line;
        if (info.spec_revision) {
            status.has_spec_revision = true;
            status.spec_revision = info.spec_revision;
        }
    }
    if (s.role == USBPD_APP_ROLE_SINK) {
        status.has_typec_current = true;
        status.typec_current = static_cast<TypeCCurrent>(s.rp);   // same numbering (static_assert below)
    }
    if (s.contract) {
        status.has_contract_voltage = true;
        status.contract_voltage = static_cast<float>(s.contract_mv) / 1000.0f;
        status.has_contract_current = true;
        status.contract_current = static_cast<float>(s.contract_ma) / 1000.0f;
    }
    status.source_pdos_count = static_cast<pb_size_t>(std::min<uint32_t>(st.caps_count, 7u));
    std::memcpy(status.source_pdos, st.caps, status.source_pdos_count * sizeof(uint32_t));
    if (st.selected) {
        status.has_selected_pdo = true;
        status.selected_pdo = st.selected;
    }
    status.source_state = source_state_.load(std::memory_order_relaxed);
    status.has_dead_battery_rd = true;
    status.dead_battery_rd = HAL_GPIO_ReadPin(g_board_usb_dead_batt_disable.GPIOx,
                                              static_cast<uint16_t>(g_board_usb_dead_batt_disable.pin)) ==
                             GPIO_PIN_RESET;
    status.has_hard_resets = true;
    status.hard_resets = s.hard_resets;
    if (const uint32_t ma = usb_limit_ma_.load(std::memory_order_relaxed); ma) {
        status.has_input_current_limit = true;
        status.input_current_limit = static_cast<float>(ma) / 1000.0f;
    }
    status.has_vbus_voltage = true;
    status.vbus_voltage = static_cast<float>(vbus_mv()) / 1000.0f;
}

void UsbPdPort::fill_config(UsbPdConfig& config) const noexcept {
    const Config cfg = this->config();
    config = UsbPdConfig_init_zero;
    config.source_enabled = cfg.source_enabled;
    config.max_sink_voltage = static_cast<float>(cfg.max_sink_mv) / 1000.0f;
    config.max_sink_current = static_cast<float>(cfg.max_sink_ma) / 1000.0f;
    config.has_source_min_battery_voltage = true;
    config.source_min_battery_voltage = static_cast<float>(cfg.source_min_vbat_mv) / 1000.0f;
}

std::expected<void, ControllerError> UsbPdPort::set_config(const UsbPdConfigRequest& request) noexcept {
    if ((request.has_max_sink_voltage && !in_range(request.max_sink_voltage, 5.0f, 20.0f)) ||
        (request.has_max_sink_current && !in_range(request.max_sink_current, 0.5f, 3.0f)) ||
        (request.has_source_min_battery_voltage && !in_range(request.source_min_battery_voltage, 9.0f, 17.0f))) {
        return std::unexpected(ControllerError::USBPD_InvalidParameter);
    }
    bool sink_changed = false;
    bool sink_attached = false;
    {
        Lock lock;
        if (request.has_source_enabled) {
            cfg_.source_enabled = request.source_enabled;
        }
        if (request.has_max_sink_voltage) {
            const auto mv = static_cast<uint32_t>(request.max_sink_voltage * 1000.0f + 0.5f);
            sink_changed |= mv != cfg_.max_sink_mv;
            cfg_.max_sink_mv = mv;
        }
        if (request.has_max_sink_current) {
            const auto ma = static_cast<uint32_t>(request.max_sink_current * 1000.0f + 0.5f);
            sink_changed |= ma != cfg_.max_sink_ma;
            cfg_.max_sink_ma = ma;
        }
        if (request.has_source_min_battery_voltage) {
            cfg_.source_min_vbat_mv = static_cast<uint32_t>(request.source_min_battery_voltage * 1000.0f + 0.5f);
        }
        sink_attached = st_.s.attached && st_.s.role == USBPD_APP_ROLE_SINK && st_.pd_capable;
    }
    wake();
    if (sink_changed && sink_attached) {
        // New limits apply to the next Request: fetch the partner's capabilities again.
        if (const int r = usbpd_port_renegotiate(false); r != 0) {
            os::warn("USB-PD: renegotiation after a config change failed (%d)\n", r);
        }
    }
    return {};
}

std::expected<void, ControllerError> UsbPdPort::renegotiate(const UsbPdRenegotiate& request) noexcept {
    switch (usbpd_port_renegotiate(request.has_hard_reset && request.hard_reset)) {
        case 0: return {};
        case 1: return std::unexpected(ControllerError::USBPD_NotAttached);
        case 2: return std::unexpected(ControllerError::USBPD_StackBusy);
        default: return std::unexpected(ControllerError::USBPD_StackError);
    }
}

static_assert(static_cast<int>(TypeCCurrent_TYPEC_CURRENT_DEFAULT) == USBPD_APP_RP_DEFAULT &&
              static_cast<int>(TypeCCurrent_TYPEC_CURRENT_1A5) == USBPD_APP_RP_1A5 &&
              static_cast<int>(TypeCCurrent_TYPEC_CURRENT_3A0) == USBPD_APP_RP_3A0,
              "TypeCCurrent mirrors usbpd_app_rp_t");

// =============================================================================
// C bridge (headers/usbpd_app.h)
// =============================================================================

extern "C" {

uint32_t usbpd_app_vbus_mv(void) { return g_port.vbus_mv(); }
bool usbpd_app_source_allowed(void) { return g_port.source_allowed(); }
bool usbpd_app_attached(usbpd_app_role_t role, uint8_t cc_line, usbpd_app_rp_t rp) {
    return g_port.attached(role, cc_line, rp);
}
void usbpd_app_detached(void) { g_port.detached(); }
void usbpd_app_source_caps(const uint32_t* pdos, uint32_t count) { g_port.source_caps(pdos, count); }
uint32_t usbpd_app_build_request(bool* is_apdo) {
    if (is_apdo) {
        *is_apdo = false;
    }
    return g_port.build_request();
}
uint32_t usbpd_app_sink_pdos(uint32_t* pdos, uint32_t max) { return g_port.sink_pdos(pdos, max); }
uint32_t usbpd_app_source_pdos(uint32_t* pdos, uint32_t max) {
    if (max == 0) {
        return 0;
    }
    pdos[0] = usb_pd::kSourcePdo;
    return 1;
}
usbpd_app_request_result_t usbpd_app_evaluate_request(uint32_t rdo) { return g_port.evaluate_request(rdo); }
void usbpd_app_event(usbpd_app_event_t event) { g_port.event(event); }
void usbpd_app_hard_reset(usbpd_app_role_t role, usbpd_app_hard_reset_t phase) { g_port.hard_reset(role, phase); }

} // extern "C"

#endif // resonant_converter_controller
