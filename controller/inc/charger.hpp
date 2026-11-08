#ifndef __CHARGER_HPP__
#define __CHARGER_HPP__

#include "bq25792.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include "error.hpp"
#include <cstdint>
#include <expected>

/** Ship FET Q3 is fitted on SDRV (controller board): enables ship mode (power off) */
#ifndef CHARGER_SHIP_FET_PRESENT
#define CHARGER_SHIP_FET_PRESENT 1
#endif

/**
 * @brief Host input settings: inputs to the PowerPath decision, stored by Charger only
 *        (set_config / ChargerConfigRequest). Charger never writes them to the chip.
 */
struct ChargerInputSettings {
    /** host input_source: AUTO = dock (VAC2) first, USB-C (VAC1) otherwise; AC1 / AC2 = forced */
    ChargerInputSource source_mode = ChargerInputSource_CHARGER_INPUT_AUTO;
    /** A, IINDPM while the dock (VAC2) supplies, before the cap (host dock_input_current_limit) */
    float dock_limit_a = 3.0f;
    /** A, upper bound on IINDPM for any input (host input_current_limit; 3.3 = none) */
    float current_cap_a = 3.3f;
    /** V, lower bound on VINDPM for any input (host input_voltage_limit; 3.6 = none) */
    float voltage_floor_v = 3.6f;
};

/**
 * @brief Charger::poll_inputs result. One bus transaction; nothing here reads the
 *        clear-on-read flag registers (REG22-REG27).
 */
struct ChargerInputPoll {
    // ---- always read: REG1B and the VAC1/VAC2/VBAT ADC burst ----
    bool vbus_present = false;      // REG1B VBUS_PRESENT_STAT
    bool ac1_present = false;       // REG1B AC1_PRESENT_STAT: VAC1 (USB-C) > 3.4 V typ
    bool ac2_present = false;       // REG1B AC2_PRESENT_STAT: VAC2 (dock) > 3.4 V typ
    bool power_good = false;        // REG1B PG_STAT
    bool poor_source = false;       // REG1B POORSRC_STAT
    bool in_vindpm = false;         // REG1B VINDPM_STAT (VOTG regulation while in OTG)
    bool in_iindpm = false;         // REG1B IINDPM_STAT (IOTG regulation while in OTG)
    uint32_t vac1_mv = 0;           // VAC1 ADC: USB-C VBUS at the connector side of the FETs
    uint32_t vac2_mv = 0;           // VAC2 ADC: dock input
    uint32_t vbat_mv = 0;           // VBAT ADC (BATP sense)
    // ---- full poll only (poll_inputs(..., true)); false when not read ----
    bool full = false;              // the fields below were read in this call
    bool battery_present = false;   // REG1D VBAT_PRESENT_STAT
    bool otg_enabled = false;       // REG12 EN_OTG
    bool en_acdrv1 = false;         // REG13 EN_ACDRV1: VAC1 path on (the chip updates it on its own swaps)
    bool en_acdrv2 = false;         // REG13 EN_ACDRV2: VAC2 path on
    bool dis_acdrv = false;         // REG12 DIS_ACDRV: both paths forced off
    bool vbat_otg_low = false;      // REG1F VBATOTG_LOW_STAT (fixed 2.8 V threshold, not per cell)
    bool ts_cold = false;           // REG1F TS_COLD_STAT
    bool ts_hot = false;            // REG1F TS_HOT_STAT
    bool otg_fault = false;         // REG21 OTG_OVP_STAT or OTG_UVP_STAT
    bool otg_ovp = false;           // REG21 OTG_OVP_STAT
    bool otg_uvp = false;           // REG21 OTG_UVP_STAT
    bool vac1_ovp = false;          // REG20 VAC1_OVP_STAT
    bool vac2_ovp = false;          // REG20 VAC2_OVP_STAT
    // ---- always ----
    bool watchdog_reapplied = false;  // WD_STAT was 1 (expiry or chip reset): everything was re-applied in this call
    ChargerInputSettings settings;    // host input settings (copied, no I2C)
};

/**
 * @brief What PowerPath wants the input stage to be (Charger::apply_power_path).
 *        Charger keeps the last target and re-applies it after a watchdog expiry.
 */
struct PowerPathTarget {
    /** AC1 / AC2: select that input (one EN_ACDRV write); AUTO: leave the mux alone.
        Ignored while otg is true (OTG forces EN_ACDRV1=1, EN_ACDRV2=0). */
    ChargerInputSource input = ChargerInputSource_CHARGER_INPUT_AUTO;
    /** A, IINDPM to program (cap already applied); clamped to 0.1-3.3 */
    float iindpm_a = 0.5f;
    /** V, VINDPM to program (floor already applied); clamped to 3.6-22 */
    float vindpm_v = 3.6f;
    /** OTG (USB-C source from the battery onto VAC1) on */
    bool otg = false;
    /** V, VOTG while otg (2.8-22, written when OTG is on) */
    float otg_voltage_v = 5.0f;
    /** A, IOTG while otg (0.12-3.32, written when OTG is on) */
    float otg_current_a = 3.2f;
};

/**
 * @brief Charger management (BQ25792), owner of its registers
 *
 * Every public method runs as one transaction on the shared internal I2C bus
 * (i2c_internal_bus_transaction): it holds the bus lock, and on a bus fault the
 * bus is recovered and the operation retried once. Private *_locked helpers
 * assume the lock is held; the lock also guards all cached state.
 *
 * Watchdog: WD_STAT=1 means the chip is in default mode after a watchdog expiry or
 * a power-on reset. Any register write restarts the watchdog and clears WD_STAT,
 * so it is checked by service(), by poll_inputs() and before every write. On
 * WD_STAT=1 the watchdog is kicked and everything is re-applied: ADC setup, the
 * input policy bits (initialize()), the host charge settings (VREG/ICHG/EN_CHG)
 * and the last PowerPathTarget (VOTG/IOTG/EN_OTG are reset by the watchdog).
 *
 * Input policy (docs/usb_pd.md): PowerPath decides the active input, IINDPM,
 * VINDPM and OTG and writes them through apply_power_path(). The host input
 * settings are only stored and handed to PowerPath (poll_inputs().settings).
 */
class Charger {
public:
    explicit Charger(I2C_HandleTypeDef* hi2c);
    ~Charger();

    /**
     * @brief Find the chip, take it into host mode, set up the ADC (D+/D- channels off)
     *        and the input policy bits: IINDPM := limit in use (REG19), EN_EXTILIM=0,
     *        AUTO_INDET_EN=0, HVDCP_EN=0, EN_IBAT=1, SFET_PRESENT (CHARGER_SHIP_FET_PRESENT).
     *        VINDPM and the mux are left alone.
     */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Periodic housekeeping (call every few seconds, < 20 s):
     *        retries initialisation if the charger was not found at boot, kicks the
     *        BQ25792 I2C watchdog, re-applies everything after a watchdog expiry, and
     *        retries charge settings or a PowerPathTarget whose write failed part-way.
     */
    std::expected<void, ControllerError> service() noexcept;

    // ---- Input power path (PowerPath thread, docs/usb_pd.md "Interfaces") ----

    /**
     * @brief Read the input status: REG1B + VAC1/VAC2/VBAT ADC (8 bytes); with full,
     *        also REG12-13 and REG1D-21. Handles WD_STAT=1 (re-applies everything and
     *        sets poll.watchdog_reapplied). Fills poll.settings from the host settings.
     *        A full poll that finds the mux differs from the last written input (the
     *        chip swapped on its own) makes the next apply_power_path() write it again.
     * @return BQ25792_NotInitialized (not logged) until service() has found the chip
     */
    std::expected<void, ControllerError> poll_inputs(ChargerInputPoll& poll, bool full) noexcept;

    /**
     * @brief Store the target and write what differs from the last successful write.
     *
     * Order: limits that go down, input mux, limits that go up (so the input never
     * runs above the new limits); then OTG: on = VOTG, IOTG, EN_OTG=1, then
     * EN_ACDRV1=1/EN_ACDRV2=0 (Table 8-8: the converter waits for EN_ACDRV1);
     * off = EN_OTG=0. A failed write keeps the target dirty: the next call or
     * service() retries it.
     */
    std::expected<void, ControllerError> apply_power_path(const PowerPathTarget& target) noexcept;

    /** @brief VAC1 pull-down (REG16 VAC1_PD_EN, ~60 Ohm): discharge VBUS after the source stops */
    std::expected<void, ControllerError> set_vac1_discharge(bool on) noexcept;

    // ---- Ship mode (ship FET Q3 on SDRV, datasheet 8.3.12) ----

    /** @brief VBUS pull-down (REG16 VBUS_PD_EN, 6 kOhm): drain VBUS before ship mode */
    std::expected<void, ControllerError> set_vbus_discharge(bool on) noexcept;

    /**
     * @brief SDRV_CTRL = ship mode, 10 s after the write (SDRV_DLY=0). The chip refuses it
     *        while VBUS is high; the read back tells.
     * @return true: accepted (the battery is cut off in 10 s unless cancelled or an
     *         adapter is plugged in); false: refused
     */
    std::expected<bool, ControllerError> enter_ship_mode() noexcept;
    /** @brief SDRV_CTRL = IDLE (cancels a pending ship mode) */
    std::expected<void, ControllerError> cancel_ship_mode() noexcept;
    /** @brief SDRV_CTRL reads ship mode (a pending one included) */
    std::expected<bool, ControllerError> ship_mode_pending() const noexcept;

    // ---- Charging ----

    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /**
     * @brief Store the host input selection (AC1 = USB-C, AC2 = dock, AUTO = dock first).
     *        PowerPath applies it; nothing is written here.
     */
    std::expected<void, ControllerError> set_input_source(ChargerInputSource source) noexcept;
    /** @brief Active input read from the mux (DIS_ACDRV/EN_ACDRV1/2) */
    std::expected<ChargerInputSource, ControllerError> get_input_source() const noexcept;

    std::expected<void, ControllerError> get_status(ChargerStatus& status) const noexcept;
    std::expected<void, ControllerError> get_measurements(ChargerMeasurements& measurements) const noexcept;
    /** @brief VREG/ICHG/VINDPM/IINDPM in use (read back) + host dock limit, cap and floor */
    std::expected<void, ControllerError> get_config(ChargerConfig& config) const noexcept;

    std::expected<void, ControllerError> get_status(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_measurements(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_config(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_full_status(SystemStatus& status) const noexcept;

    /**
     * @brief Apply all fields (validated before any write). input_voltage_limit and
     *        input_current_limit set the floor and the cap, unless input_voltage_floor /
     *        input_current_cap are present, which take precedence (so a get_config()
     *        result can be written back unchanged).
     */
    std::expected<void, ControllerError> set_config(const ChargerConfig& config) noexcept;
    /**
     * @brief Apply only the fields present (has_*); validated before any write.
     *        VREG/ICHG/enabled are written to the chip; input_voltage_limit (VINDPM
     *        floor), input_current_limit (cap), dock_input_current_limit and
     *        input_source are stored for PowerPath.
     */
    std::expected<void, ControllerError> set_config(const ChargerConfigRequest& config) noexcept;

    bool has_updates() const noexcept;
    bool wait_for_update(uint32_t timeout_ms) const noexcept;
    void trigger_update() noexcept;
    uint32_t get_timestamp_ms() const noexcept { return last_update_ms_; }

    bool is_initialized() const noexcept;
    std::expected<bool, ControllerError> is_plugged_in() const noexcept;
    std::expected<bool, ControllerError> is_battery_present() const noexcept;
    std::expected<bool, ControllerError> is_fault_present() const noexcept;
    std::expected<ChargeStatus, ControllerError> get_charge_status() const noexcept;
    std::expected<VBUSStatus, ControllerError> get_vbus_status() const noexcept;
    std::expected<float, ControllerError> get_vbus_voltage() const noexcept;
    std::expected<float, ControllerError> get_vsys_voltage() const noexcept;
    std::expected<float, ControllerError> get_vbat_voltage() const noexcept;
    std::expected<float, ControllerError> get_ibus_current() const noexcept;
    std::expected<float, ControllerError> get_ibat_current() const noexcept;

private:
    // Charge settings requested by the host; only fields the host has set are written.
    // ICHG and EN_CHG are reset by the watchdog (VREG per section 8.3.2 too), so they are
    // re-applied after every expiry.
    struct Settings {
        bool has_charge_voltage_limit = false;
        float charge_voltage_limit = 0.0f;
        bool has_charge_current_limit = false;
        float charge_current_limit = 0.0f;
        bool has_enabled = false;
        bool enabled = true;
    };

    // Values last written successfully by apply_power_path, in register steps; a
    // *_known flag is false when the chip value is unknown (boot, failed write,
    // watchdog reset), which forces the next write.
    struct PowerPathWritten {
        bool input_known = false;
        ChargerInputSource input = ChargerInputSource_CHARGER_INPUT_AUTO;  // last mux write (AC1/AC2)
        bool iindpm_known = false;
        uint32_t iindpm_ma = 0;
        bool vindpm_known = false;
        uint32_t vindpm_mv = 0;
        bool otg_known = false;
        bool otg = false;
        bool votg_known = false;
        uint32_t votg_mv = 0;
        bool iotg_known = false;
        uint32_t iotg_ma = 0;
    };

    bq25792::Charger driver_;
    os::semaphore update_sem_;
    mutable uint32_t last_update_ms_;

    Settings settings_;
    bool settings_dirty_ = false;     // settings_ not (completely) applied to the chip
    ChargerInputSettings input_settings_;

    bool has_target_ = false;         // apply_power_path() was called at least once
    PowerPathTarget target_;
    bool target_dirty_ = false;       // target_ not (completely) written
    PowerPathWritten written_;
    bool vac1_discharge_ = false;     // last requested VAC1 pull-down state
    bool vbus_discharge_ = false;     // last requested VBUS pull-down state

    bool reapply_pending_ = false;    // chip reset/expiry seen, full re-apply not finished
    bool reapplied_unreported_ = false;  // a re-apply finished; reported by the next poll_inputs()

    // Mux state seen by the last full poll, to notice the chip's own input swaps
    bool mux_seen_known_ = false;
    bool mux_seen_acdrv1_ = false;
    bool mux_seen_acdrv2_ = false;

    // What apply_power_path_locked() has to write for target_, in register steps
    struct PowerPathPlan {
        uint32_t iindpm_ma = 0;
        uint32_t vindpm_mv = 0;
        uint32_t votg_mv = 0;
        uint32_t iotg_ma = 0;
        ChargerInputSource mux_input = ChargerInputSource_CHARGER_INPUT_AUTO;
        bool iindpm = false, iindpm_first = false;   // write; before the mux change
        bool vindpm = false, vindpm_first = false;
        bool votg = false, iotg = false;
        bool otg_on = false, otg_off = false;
        bool mux = false;
        bool any() const noexcept { return iindpm || vindpm || votg || iotg || otg_on || otg_off || mux; }
    };
    PowerPathPlan plan_power_path_locked() const noexcept;

    mutable ErrorLogLimiter log_limiter_{5000};  // same error from the same place: log at most every 5 s

    // ---- helpers: caller holds the bus lock ----
    std::expected<void, ControllerError> apply_settings_locked(const Settings& s) noexcept;
    std::expected<void, ControllerError> apply_policy_bits_locked() noexcept;
    std::expected<void, ControllerError> reapply_all_locked() noexcept;
    std::expected<void, ControllerError> handle_watchdog_locked(bool wd_expired) noexcept;
    std::expected<void, ControllerError> check_watchdog_locked() noexcept;
    std::expected<void, ControllerError> apply_power_path_locked() noexcept;
    std::expected<void, ControllerError> write_iindpm_locked(uint32_t ma) noexcept;
    std::expected<void, ControllerError> write_vindpm_locked(uint32_t mv) noexcept;
    std::expected<void, ControllerError> write_mux_locked(ChargerInputSource input) noexcept;
    std::expected<void, ControllerError> read_status_locked(ChargerStatus& status) const noexcept;
    std::expected<void, ControllerError> read_measurements_locked(ChargerMeasurements& m) const noexcept;
    std::expected<void, ControllerError> read_config_locked(ChargerConfig& config) const noexcept;
    void store_input_settings_locked(const ChargerConfigRequest& config) noexcept;

    /** @brief Map a driver error, log it (rate limited) and return it as unexpected */
    std::unexpected<ControllerError> fail(const char* what, const std::error_code& ec) const noexcept;

    template <typename T>
    std::expected<T, ControllerError> check(const char* what, bq25792::expected<T> r) const noexcept {
        if (!r) {
            return fail(what, r.error());
        }
        if constexpr (std::is_void_v<T>) {
            return {};
        } else {
            return std::move(*r);
        }
    }
};

#endif
