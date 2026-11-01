#ifndef __USBPD_APP_HPP__
#define __USBPD_APP_HPP__

#include "error.hpp"
#include "protocol.pb.h"
#include "usbpd_app.h"
#include <atomic>
#include <cstdint>
#include <expected>

/**
 * @brief USB-C port state shared between the ST USB-PD stack (usbpd_app_* callbacks in
 *        the CAD thread, PE thread and UCPD ISR), PowerPath (usbpd-power thread) and the
 *        host (message handler). docs/usb_pd.md.
 *
 * Callbacks only touch this state inside a short PRIMASK critical section and wake
 * PowerPath with osThreadFlagsSet (ISR safe): no I2C, no logging. The object is
 * constant-initialised so the stack can call in before controller_system_init runs.
 */
class UsbPdPort {
public:
    struct Config {
        bool source_enabled = true;
        uint32_t max_sink_mv = 20000;
        uint32_t max_sink_ma = 3000;
        uint32_t source_min_vbat_mv = 13200;   // 4S pack: 3.3 V per cell
    };

    /** @brief Port state as PowerPath needs it (copied under the lock) */
    struct Snapshot {
        bool attached = false;
        usbpd_app_role_t role = USBPD_APP_ROLE_NONE;
        uint8_t cc_line = 0;
        usbpd_app_rp_t rp = USBPD_APP_RP_NONE;
        uint32_t attach_ms = 0;
        bool contract = false;             // explicit contract (sink: contract_* is what we draw)
        uint32_t contract_mv = 0;
        uint32_t contract_ma = 0;
        bool request_in_flight = false;    // sink: Request sent, PS_RDY not yet seen
        uint32_t request_mv = 0;
        uint32_t request_ma = 0;
        bool hard_reset = false;           // hard reset in progress
        bool source_vbus = false;          // source: VBUS wanted on
        bool discharge = false;            // source: VBUS was switched off by a hard reset (consumed)
        uint32_t hard_resets = 0;
    };

    // ---- PowerPath ----
    void set_wake_thread(void* thread_id) noexcept { wake_thread_.store(thread_id); }
    Snapshot take_snapshot() noexcept;   // clears the one-shot discharge flag
    Config config() const noexcept;
    void set_vbus_mv(uint32_t mv) noexcept { vbus_mv_.store(mv, std::memory_order_relaxed); }
    void set_source_allowed(bool allowed) noexcept { source_allowed_.store(allowed, std::memory_order_relaxed); }
    void set_source_state(UsbPdSourceState state) noexcept { source_state_.store(state, std::memory_order_relaxed); }
    void set_dock_present(bool present) noexcept { dock_present_.store(present, std::memory_order_relaxed); }
    void set_usb_limit_ma(uint32_t ma) noexcept { usb_limit_ma_.store(ma, std::memory_order_relaxed); }

    // ---- Host (message handler thread) ----
    void fill_status(UsbPdStatus& status) noexcept;
    void fill_config(UsbPdConfig& config) const noexcept;
    std::expected<void, ControllerError> set_config(const UsbPdConfigRequest& request) noexcept;
    std::expected<void, ControllerError> renegotiate(const UsbPdRenegotiate& request) noexcept;

    // ---- Stack callbacks (usbpd_app_* in usbpd_app.cc) ----
    uint32_t vbus_mv() const noexcept { return vbus_mv_.load(std::memory_order_relaxed); }
    bool source_allowed() const noexcept { return source_allowed_.load(std::memory_order_relaxed); }
    bool attached(usbpd_app_role_t role, uint8_t cc_line, usbpd_app_rp_t rp) noexcept;
    void detached() noexcept;
    void source_caps(const uint32_t* pdos, uint32_t count) noexcept;
    uint32_t build_request() noexcept;
    uint32_t sink_pdos(uint32_t* pdos, uint32_t max) const noexcept;
    usbpd_app_request_result_t evaluate_request(uint32_t rdo) noexcept;
    void event(usbpd_app_event_t event) noexcept;
    void hard_reset(usbpd_app_role_t role, usbpd_app_hard_reset_t phase) noexcept;

private:
    void wake() noexcept;

    struct State {
        Snapshot s;
        uint32_t caps[7] = {};
        uint32_t caps_count = 0;
        uint32_t selected = 0;             // 1-based position of the last request
        bool pd_capable = true;            // false after PE_DISABLED (Type-C only partner)
    };

    State st_{};
    Config cfg_{};
    std::atomic<void*> wake_thread_{nullptr};
    std::atomic<uint32_t> vbus_mv_{0};
    std::atomic<bool> source_allowed_{false};
    std::atomic<bool> dock_present_{false};
    std::atomic<uint32_t> usb_limit_ma_{0};
    std::atomic<UsbPdSourceState> source_state_{UsbPdSourceState_USB_PD_SOURCE_DISABLED};
};

/** @brief The board's USB-C port (resonant_converter_controller only) */
UsbPdPort& usb_pd_port() noexcept;

#endif
