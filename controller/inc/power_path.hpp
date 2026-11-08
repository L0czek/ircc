#ifndef __POWER_PATH_HPP__
#define __POWER_PATH_HPP__

#include "charger.hpp"
#include "output_voltage_regulator.hpp"
#include "usbpd_app.hpp"
#include "cmsis_os2.h"
#include <atomic>
#include <cstdint>

/**
 * @brief Input power policy: active input, IINDPM / VINDPM and the USB-C source, from the
 *        BQ25792 input status, the TPS55289 VOUT state and the USB-C port state.
 *        docs/usb_pd.md.
 *
 * USB-C source paths (5 V 3 A):
 *  - dock present: VOUT through the U26 eFuse (PC13), only while VOUT already is 5 V,
 *    enabled, >= 3 A and fault free. VOUT is never changed for USB; a VOUT change that
 *    breaks this opens the bridge before it is written (vout_changing()).
 *  - no dock: BQ25792 OTG from the battery.
 * The path is fixed when VBUS turns on; a change of the wanted path ends the source role
 * (error recovery) and the next attach gets the new one.
 *
 * Power off (ship mode, docs/lcd_ui.md "Power off"): request_ship_mode() stops the USB-C
 * source and waits until USB-C (VAC1) and the dock (VAC2) are unplugged. Then it drains
 * VBUS (VBUS_PD_EN) and writes SDRV_CTRL = ship mode with the 10 s delay; the ship FET Q3
 * cuts the battery off. The charger refuses ship mode while VBUS is high, and puts it back
 * to IDLE when an adapter is plugged in during the delay: both send it back to waiting.
 *
 * Runs in its own thread ("usbpd-power", static memory, osPriorityNormal1): every 20 ms,
 * or at once when the port or the VOUT listener signals an event. All chip access goes
 * through Charger and OutputVoltageRegulator.
 */
class PowerPath final : public VoutListener {
public:
    PowerPath(Charger& charger, OutputVoltageRegulator& vout, UsbPdPort& port) noexcept
        : charger_(charger), vout_(vout), port_(port) {}

    /** @brief Register as VOUT listener and start the thread (once) */
    void start() noexcept;

    /** @brief VoutListener: opens the bridge before a VOUT change it cannot follow */
    void vout_changing(const VoutState& next) noexcept override;

    /** @brief Start a ship-mode power off (any thread; runs in the PowerPath thread) */
    void request_ship_mode() noexcept;
    /** @brief Abandon a ship-mode power off (cancels a pending SDRV_CTRL write) */
    void cancel_power_off() noexcept;
    /** @brief Fill the power off status; false while idle (any thread) */
    bool power_off_status(PowerOffStatus& out) const noexcept;

private:
    enum class OffRequest : uint8_t { None, Start, Cancel };
    enum class Path : uint8_t { None, Otg, Vout };

    static void thread_main(void* arg);
    void run() noexcept;
    void step(bool full) noexcept;
    void poll_vout() noexcept;
    Path update_source(const ChargerInputPoll& poll, const UsbPdPort::Config& cfg, bool sourcing) noexcept;
    void step_power_off(const ChargerInputPoll& poll, bool full, uint32_t now) noexcept;
    void set_off_state(PowerOffState state) noexcept;
    void wake() noexcept;

    // Bridge GPIO (PC13). Closed only by this thread; opened by it or by vout_changing().
    bool close_bridge() noexcept;      // false if VOUT changed since poll_vout()
    void open_bridge() noexcept;
    static bool bridge_pin_high() noexcept;

    Charger& charger_;
    OutputVoltageRegulator& vout_;
    UsbPdPort& port_;
    osThreadId_t thread_ = nullptr;

    // ---- VOUT ----
    std::atomic<uint32_t> vout_seq_{0};       // bumped by vout_changing()
    std::atomic<bool> bridge_tripped_{false}; // vout_changing() opened the bridge
    uint32_t vout_seq_polled_ = 0;            // vout_seq_ when vout_state_ was read
    VoutState vout_state_{};
    bool vout_fits_ = false;
    bool vout_failed_ = false;                // VBUS did not come up through the bridge
    uint32_t bridge_closed_ms_ = 0;           // when the bridge was closed (0 = open)

    // ---- Latest full-poll values (refreshed every ~200 ms) ----
    bool battery_present_ = false;
    bool vbat_otg_low_ = false;
    bool en_acdrv2_ = false;
    bool otg_fault_ = false;              // latched while sourcing, cleared on detach

    // ---- Source ----
    Path active_ = Path::None;            // path driving VBUS (fixed until VBUS goes off)
    bool recovery_requested_ = false;     // error recovery sent for the current attach
    uint32_t discharge_until_ms_ = 0;     // VAC1 pull-down running while non-zero
    bool discharge_on_ = false;

    // ---- Input ----
    uint32_t ac1_since_ms_ = 0;           // VAC1 present since (0 = absent)
    uint32_t dock_vindpm_mv_ = 0;         // VINDPM for the dock, measured when it was selected
    bool dock_warned_ = false;

    bool have_target_ = false;
    bool apply_failed_ = false;
    PowerPathTarget last_{};

    // ---- Power off (state and flags are read by other threads) ----
    std::atomic<OffRequest> off_request_{OffRequest::None};
    std::atomic<PowerOffState> off_state_{PowerOffState_POWER_OFF_IDLE};
    std::atomic<uint8_t> off_inputs_{0};          // kOff* bits from the last poll
    std::atomic<uint32_t> off_deadline_ms_{0};    // PENDING: when the ship FET turns off
    uint32_t next_ship_try_ms_ = 0;
    bool vbus_discharge_on_ = false;
    PowerOffState logged_off_state_ = PowerOffState_POWER_OFF_IDLE;

    // ---- Logged on change ----
    bool logged_contract_ = false;
    uint32_t logged_contract_mv_ = 0;
    uint32_t logged_contract_ma_ = 0;
    ChargerInputSource logged_input_ = ChargerInputSource_CHARGER_INPUT_AUTO;
    UsbPdSourceState logged_source_ = UsbPdSourceState_USB_PD_SOURCE_DISABLED;
    Path logged_path_ = Path::None;
};

#endif
