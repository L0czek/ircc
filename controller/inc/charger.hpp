#ifndef __CHARGER_HPP__
#define __CHARGER_HPP__

#include "bq25792.hpp"
#include "mutex.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include "error.hpp"
#include <cstdint>
#include <mutex>
#include <expected>

/**
 * @brief Charger management class
 *
 * Wraps the BQ25792 driver with thread-safe access using mutexes.
 * Provides comprehensive status reporting for TUI display.
 */
class Charger {
    bq25792::Charger driver_;
    mutable os::mutex mutex_;
    os::semaphore update_sem_;

    // Cached status for fast TUI updates
    mutable ChargerStatus cached_status_;
    mutable ChargerMeasurements cached_measurements_;
    mutable uint32_t last_update_ms_;

public:
    /**
     * @brief Construct charger manager
     * @param hi2c I2C handle for BQ25792 communication
     */
    explicit Charger(I2C_HandleTypeDef* hi2c);

    /**
     * @brief Destroy charger manager
     */
    ~Charger();

    /**
     * @brief Initialize the charger
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Enable/disable the charger
     * @param enable true to enable, false to disable
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;

    /**
     * @brief Check if charger is enabled
     * @return std::expected<bool, ControllerError> - true if enabled, error on failure
     */
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /**
     * @brief Set input source selection
     * @param source Charger input source (USB_DPDM, AC1, AC2, AUTO)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_input_source(ChargerInputSource source) noexcept;

    /**
     * @brief Get current input source selection
     * @return ChargerInputSource enum
     */
    ChargerInputSource get_input_source() const noexcept;

    /**
     * @brief Get comprehensive charger status for TUI
     * @param status Reference to ChargerStatus message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_status(ChargerStatus& status) const noexcept;

    /**
     * @brief Get charger measurements for TUI
     * @param measurements Reference to ChargerMeasurements message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_measurements(ChargerMeasurements& measurements) const noexcept;

    /**
     * @brief Get full charger configuration
     * @param config Reference to ChargerConfig message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_config(ChargerConfig& config) const noexcept;

    /**
     * @brief Set charger configuration
     * @param config Charger configuration to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_config(const ChargerConfig& config) noexcept;

    /**
     * @brief Set charger configuration from request
     * @param config Charger configuration request to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_config(const ChargerConfigRequest& config) noexcept;

    /**
     * @brief Check if charger has new data available
     * @return true if updates available, false otherwise
     */
    bool has_updates() const noexcept;

    /**
     * @brief Wait for charger update with timeout
     * @param timeout_ms Timeout in milliseconds (0 for infinite)
     * @return true if update available, false on timeout
     */
    bool wait_for_update(uint32_t timeout_ms) const noexcept;

    /**
     * @brief Trigger an update
     */
    void trigger_update() noexcept;

    /**
     * @brief Get cached status timestamp
     * @return Timestamp in milliseconds
     */
    uint32_t get_timestamp_ms() const noexcept { return last_update_ms_; }

    /**
     * @brief Check if charger is initialized
     * @return std::expected<bool, ControllerError> - true if initialized, error on failure
     */
    std::expected<bool, ControllerError> is_initialized() const noexcept;

    /**
     * @brief Check if input (VBUS) is plugged in
     * @return std::expected<bool, ControllerError> - true if VBUS present, error on failure
     */
    std::expected<bool, ControllerError> is_plugged_in() const noexcept;

    /**
     * @brief Check if battery is present
     * @return std::expected<bool, ControllerError> - true if battery present, error on failure
     */
    std::expected<bool, ControllerError> is_battery_present() const noexcept;

    /**
     * @brief Check if fault condition exists
     * @return std::expected<bool, ControllerError> - true if fault present, error on failure
     */
    std::expected<bool, ControllerError> is_fault_present() const noexcept;

    /**
     * @brief Get charge status
     * @return ChargeStatus enum
     */
    ChargeStatus get_charge_status() const noexcept;

    /**
     * @brief Get VBUS status
     * @return VBUSStatus enum
     */
    VBUSStatus get_vbus_status() const noexcept;

    /**
     * @brief Get VBUS voltage in volts
     * @return std::expected<float, ControllerError> - VBUS voltage in volts, error on failure
     */
    std::expected<float, ControllerError> get_vbus_voltage() const noexcept;

    /**
     * @brief Get VSYS voltage in volts
     * @return std::expected<float, ControllerError> - VSYS voltage in volts, error on failure
     */
    std::expected<float, ControllerError> get_vsys_voltage() const noexcept;

    /**
     * @brief Get VBAT voltage in volts
     * @return std::expected<float, ControllerError> - VBAT voltage in volts, error on failure
     */
    std::expected<float, ControllerError> get_vbat_voltage() const noexcept;

    /**
     * @brief Get IBUS current in amps
     * @return std::expected<float, ControllerError> - IBUS current in amps (positive for charge), error on failure
     */
    std::expected<float, ControllerError> get_ibus_current() const noexcept;

    /**
     * @brief Get IBAT current in amps
     * @return std::expected<float, ControllerError> - IBAT current in amps (positive for charge), error on failure
     */
    std::expected<float, ControllerError> get_ibat_current() const noexcept;
};

#endif
