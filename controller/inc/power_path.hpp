#ifndef __POWER_PATH_HPP__
#define __POWER_PATH_HPP__

#include "charger.hpp"
#include "usbpd_app.hpp"
#include "cmsis_os2.h"
#include <cstdint>

/**
 * @brief Input power policy: active input, IINDPM / VINDPM and the USB-C source (OTG),
 *        from the BQ25792 input status and the USB-C port state. docs/usb_pd.md.
 *
 * Runs in its own thread ("usbpd-power", static memory, osPriorityNormal1): every 20 ms,
 * or at once when the port signals an event. All chip access goes through Charger.
 */
class PowerPath {
public:
    PowerPath(Charger& charger, UsbPdPort& port) noexcept : charger_(charger), port_(port) {}

    /** @brief Start the thread (once) */
    void start() noexcept;

private:
    static void thread_main(void* arg);
    void run() noexcept;
    void step(bool full) noexcept;
    bool update_source_allowed(const ChargerInputPoll& poll, const UsbPdPort::Config& cfg) noexcept;

    Charger& charger_;
    UsbPdPort& port_;
    osThreadId_t thread_ = nullptr;

    // Latest full-poll values (refreshed every ~200 ms)
    bool battery_present_ = false;
    bool vbat_otg_low_ = false;
    bool en_acdrv2_ = false;
    bool otg_fault_ = false;              // latched while sourcing, cleared on detach

    bool source_allowed_ = false;
    bool otg_ = false;                    // last OTG state we asked for
    bool recovery_requested_ = false;     // error recovery sent for the current attach
    uint32_t discharge_until_ms_ = 0;     // VAC1 pull-down running while non-zero
    bool discharge_on_ = false;

    uint32_t ac1_since_ms_ = 0;           // VAC1 present since (0 = absent)
    uint32_t dock_vindpm_mv_ = 0;         // VINDPM for the dock, measured when it was selected
    bool dock_warned_ = false;

    bool have_target_ = false;
    bool apply_failed_ = false;
    PowerPathTarget last_{};

    // Logged on change
    bool logged_contract_ = false;
    uint32_t logged_contract_mv_ = 0;
    uint32_t logged_contract_ma_ = 0;
    ChargerInputSource logged_input_ = ChargerInputSource_CHARGER_INPUT_AUTO;
    UsbPdSourceState logged_source_ = UsbPdSourceState_USB_PD_SOURCE_DISABLED;
};

#endif
