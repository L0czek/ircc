#ifndef __OUTPUT_VOLTAGE_REGULATOR_HPP__
#define __OUTPUT_VOLTAGE_REGULATOR_HPP__

#include "mutex.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include "error.hpp"
#include <cstdint>
#include <mutex>
#include <expected>

// Forward declaration of TPS55289 driver
#include "tps55289.hpp"

/**
 * @brief Output Voltage Regulator management class
 *
 * Wraps the TPS55289 driver with thread-safe access using mutexes.
 * Provides comprehensive status reporting for TUI display.
 */
class OutputVoltageRegulator {
    tps55289::TPS55289 driver_;
    mutable os::mutex mutex_;
    os::semaphore update_sem_;

    // Cached status for fast TUI updates
    mutable BackBoostStatus cached_status_;
    mutable BackBoostMeasurements cached_measurements_;
    mutable uint32_t last_update_ms_;

public:
    /**
     * @brief Construct output voltage regulator manager
     * @param hi2c I2C handle for TPS55289 communication
     */
    explicit OutputVoltageRegulator(I2C_HandleTypeDef* hi2c);

    /**
     * @brief Destroy output voltage regulator manager
     */
    ~OutputVoltageRegulator();

    /**
     * @brief Initialize the regulator
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Enable/disable the output
     * @param enable true to enable, false to disable
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;

    /**
     * @brief Check if output is enabled
     * @return bool - true if enabled (no error possible)
     */
    bool is_enabled() const noexcept;

    /**
     * @brief Set output voltage
     * @param voltage Output voltage in volts
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_output_voltage(float voltage) noexcept;

    /**
     * @brief Get output voltage
     * @return std::expected<float, ControllerError> - output voltage in volts, error on failure
     */
    std::expected<float, ControllerError> get_output_voltage() const noexcept;

    /**
     * @brief Set output current limit
     * @param current Current limit in amps
     * @param sense_resistor Sense resistor value in ohms
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_output_current_limit(float current, float sense_resistor) noexcept;

    /**
     * @brief Get output current limit
     * @param sense_resistor Sense resistor value in ohms
     * @return std::expected<float, ControllerError> - current limit in amps, error on failure
     */
    std::expected<float, ControllerError> get_output_current_limit(float sense_resistor) const noexcept;

    /**
     * @brief Get comprehensive regulator status for TUI
     * @param status Reference to BackBoostStatus message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_status(BackBoostStatus& status) const noexcept;

    /**
     * @brief Get regulator measurements for TUI
     * @param measurements Reference to BackBoostMeasurements message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_measurements(BackBoostMeasurements& measurements) const noexcept;

    /**
     * @brief Get full regulator configuration
     * @param config Reference to BackBoostConfig message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_config(BackBoostConfig& config) const noexcept;

    /**
     * @brief Set regulator configuration
     * @param config Regulator configuration to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_config(const BackBoostConfig& config) noexcept;

    /**
     * @brief Set regulator configuration from request
     * @param config Regulator configuration request to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_config(const BackBoostConfigRequest& config) noexcept;

    /**
     * @brief Check if regulator has new data available
     * @return true if updates available, false otherwise
     */
    bool has_updates() const noexcept;

    /**
     * @brief Wait for regulator update with timeout
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
     * @brief Check if regulator is initialized
     * @return bool - true if initialized (no error possible)
     */
    bool is_initialized() const noexcept;

    /**
     * @brief Check if fault condition exists
     * @return bool - true if fault present (no error possible)
     */
    bool is_fault_present() const noexcept;

    /**
     * @brief Check if short circuit fault exists
     * @return bool - true if SCP fault present (no error possible)
     */
    bool has_scp_fault() const noexcept;

    /**
     * @brief Check if over current fault exists
     * @return bool - true if OCP fault present (no error possible)
     */
    bool has_ocp_fault() const noexcept;

    /**
     * @brief Check if over voltage fault exists
     * @return bool - true if OVP fault present (no error possible)
     */
    bool has_ovp_fault() const noexcept;

    /**
     * @brief Get operating mode
     * @return BackBoostOperatingMode enum (no error possible)
     */
    BackBoostOperatingMode get_operating_mode() const noexcept;

    /**
     * @brief Get feedback mode
     * @return BackBoostFeedbackMode enum (no error possible)
     */
    BackBoostFeedbackMode get_feedback_mode() const noexcept;

    /**
     * @brief Get sense resistor value in ohms
     * @return float - sense resistor value in ohms (no error possible)
     */
    float get_sense_resistor_ohms() const noexcept;
};

#endif
