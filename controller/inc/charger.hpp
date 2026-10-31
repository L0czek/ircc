#ifndef __CHARGER_HPP__
#define __CHARGER_HPP__

#include "bq25792.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include "error.hpp"
#include <cstdint>
#include <expected>

/**
 * @brief Charger management (BQ25792)
 *
 * Every public method runs as one transaction on the shared internal I2C bus
 * (i2c_internal_bus_transaction): it holds the bus lock, and on a bus fault the
 * bus is recovered and the operation retried once. Private *_locked helpers
 * assume the lock is held.
 */
class Charger {
public:
    explicit Charger(I2C_HandleTypeDef* hi2c);
    ~Charger();

    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Periodic housekeeping (call every few seconds, < 20 s):
     *        retries initialisation if the charger was not found at boot, kicks the
     *        BQ25792 I2C watchdog, and re-applies the host settings after a watchdog
     *        expiry or after a previous apply failed part-way.
     */
    std::expected<void, ControllerError> service() noexcept;

    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /** @brief AC1 = USB-C input, AC2 = dock DC input, AUTO = first valid input */
    std::expected<void, ControllerError> set_input_source(ChargerInputSource source) noexcept;
    std::expected<ChargerInputSource, ControllerError> get_input_source() const noexcept;

    std::expected<void, ControllerError> get_status(ChargerStatus& status) const noexcept;
    std::expected<void, ControllerError> get_measurements(ChargerMeasurements& measurements) const noexcept;
    std::expected<void, ControllerError> get_config(ChargerConfig& config) const noexcept;

    std::expected<void, ControllerError> get_status(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_measurements(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_config(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_full_status(SystemStatus& status) const noexcept;

    /** @brief Apply all fields (validated before any write) */
    std::expected<void, ControllerError> set_config(const ChargerConfig& config) noexcept;
    /** @brief Apply only the fields present (has_*); validated before any write */
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
    // Settings requested by the host. The BQ25792 watchdog resets ICHG, EN_CHG
    // and ADC_EN to defaults when it expires, so service() re-applies these.
    struct Settings {
        bool has_charge_voltage_limit = false;
        float charge_voltage_limit = 0.0f;
        bool has_charge_current_limit = false;
        float charge_current_limit = 0.0f;
        bool has_input_voltage_limit = false;
        float input_voltage_limit = 0.0f;
        bool has_input_current_limit = false;
        float input_current_limit = 0.0f;
        bool has_input_source = false;
        ChargerInputSource input_source = ChargerInputSource_CHARGER_INPUT_AUTO;
        bool has_enabled = false;
        bool enabled = true;
    };

    bq25792::Charger driver_;
    os::semaphore update_sem_;
    mutable uint32_t last_update_ms_;

    Settings settings_;
    bool settings_dirty_ = false;   // settings_ not (completely) applied to the chip

    mutable ErrorLogLimiter log_limiter_{5000};  // same error from the same place: log at most every 5 s

    // ---- helpers: caller holds the bus lock ----
    std::expected<void, ControllerError> apply_settings_locked(const Settings& s) noexcept;
    std::expected<void, ControllerError> read_status_locked(ChargerStatus& status) const noexcept;
    std::expected<void, ControllerError> read_measurements_locked(ChargerMeasurements& m) const noexcept;
    std::expected<void, ControllerError> read_config_locked(ChargerConfig& config) const noexcept;

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
