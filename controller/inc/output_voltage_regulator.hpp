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
 * @brief Output Voltage Regulator management class (TPS55289)
 *
 * Wraps the TPS55289 driver with thread-safe access using mutexes.
 * Provides comprehensive status reporting for TUI display.
 */
/**
 * @brief Output voltage regulator (TPS55289) management
 *
 * All driver calls hold the shared internal-I2C bus mutex (the TPS55289 shares
 * the bus with the BQ25792 charger). Public methods lock; *_locked helpers
 * expect the caller to hold the lock.
 *
 * Note: the TPS55289 has no ADC. "Measurements" report the programmed output
 * voltage and the programmed current limit, not measured values.
 */
class OutputVoltageRegulator {
    tps55289::TPS55289 driver_;
    os::mutex& bus_mutex_;
    os::semaphore update_sem_;

    // Cached status for fast TUI updates
    mutable OutputRegulatorStatus cached_status_;
    mutable OutputRegulatorMeasurements cached_measurements_;
    mutable uint32_t last_update_ms_;

    std::expected<void, ControllerError> read_status_locked(OutputRegulatorStatus& status) const noexcept;
    std::expected<void, ControllerError> read_setpoints_locked(float& voltage, float& current_limit) const noexcept;
    std::expected<void, ControllerError> apply_config_locked(float voltage, float current_limit, float sense_resistor_ohms) noexcept;

    static ControllerError log_error(const char* what, const std::error_code& ec) noexcept;

public:
    explicit OutputVoltageRegulator(I2C_HandleTypeDef* hi2c);
    ~OutputVoltageRegulator();

    /** @brief Probe the TPS55289 and cache its feedback configuration (no register writes) */
    std::expected<void, ControllerError> initialize() noexcept;

    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /** @return programmed output voltage after quantisation */
    std::expected<float, ControllerError> set_output_voltage(float voltage) noexcept;
    std::expected<float, ControllerError> get_output_voltage() const noexcept;

    /** @param sense_resistor <= 0 uses the stored sense resistor */
    std::expected<void, ControllerError> set_output_current_limit(float current, float sense_resistor) noexcept;
    std::expected<float, ControllerError> get_output_current_limit() const noexcept;

    /** @brief Application upper bound for the output voltage */
    std::expected<void, ControllerError> set_output_voltage_limit(float max_voltage) noexcept;

    std::expected<void, ControllerError> get_status(OutputRegulatorStatus& status) const noexcept;
    std::expected<void, ControllerError> get_measurements(OutputRegulatorMeasurements& measurements) const noexcept;
    std::expected<void, ControllerError> get_config(OutputRegulatorConfig& config) const noexcept;

    std::expected<void, ControllerError> get_status(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_measurements(SystemStatus& status) const noexcept;
    std::expected<void, ControllerError> get_config(SystemStatus& status) const noexcept;

    std::expected<void, ControllerError> set_config(const OutputRegulatorConfig& config) noexcept;
    std::expected<void, ControllerError> set_config(const OutputRegulatorConfigRequest& config) noexcept;

    bool has_updates() const noexcept;
    bool wait_for_update(uint32_t timeout_ms) const noexcept;
    void trigger_update() noexcept;
    uint32_t get_timestamp_ms() const noexcept { return last_update_ms_; }

    bool is_initialized() const noexcept;
    float get_sense_resistor_ohms() const noexcept;
};

#endif
