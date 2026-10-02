#ifndef __DAC_CONTROLLER_HPP__
#define __DAC_CONTROLLER_HPP__

#include "dac.hpp"
#include "mutex.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include "error.hpp"
#include <cstdint>
#include <mutex>
#include <expected>

/**
 * @brief DAC Controller management class
 *
 * Wraps the low-level DAC driver with thread-safe access using mutexes.
 * Provides comprehensive status reporting for TUI display.
 * Supports both constant voltage mode and waveform generation mode.
 */
class DACController {
    DacDriver driver_;
    mutable os::mutex mutex_;
    os::semaphore update_sem_;

    // Cached status for fast TUI updates
    mutable DACStatus cached_status_;
    mutable DACMeasurements cached_measurements_;
    mutable uint32_t last_update_ms_;

    // Current configuration
    DACMode current_mode_;
    float current_voltage_;
    WaveformType current_waveform_type_;
    float current_frequency_;
    float current_amplitude_;

public:
    /**
     * @brief Construct DAC controller
     * @param dac_handles Array of 4 DAC_HandleTypeDef pointers (DAC2 may be nullptr for smaller packages)
     */
    explicit DACController(DAC_HandleTypeDef* dac_handles[4]);

    /**
     * @brief Destroy DAC controller
     */
    ~DACController();

    /**
     * @brief Initialize the DAC controller
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Enable/disable all DAC channels
     * @param enable true to enable, false to disable
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;

    /**
     * @brief Check if DAC channels are enabled
     * @return std::expected<bool, ControllerError> - true if enabled, error on failure
     */
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /**
     * @brief Set DAC mode (voltage or waveform)
     * @param mode DAC mode
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_mode(DACMode mode) noexcept;

    /**
     * @brief Get current DAC mode
     * @return DACMode enum
     */
    DACMode get_mode() const noexcept;

    /**
     * @brief Set output voltage (voltage mode)
     * @param voltage Output voltage in volts (0-3.3V)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_voltage(float voltage) noexcept;

    /**
     * @brief Get output voltage
     * @return std::expected<float, ControllerError> - output voltage in volts, error on failure
     */
    std::expected<float, ControllerError> get_voltage() const noexcept;

    /**
     * @brief Set waveform parameters (waveform mode)
     * @param waveform_type Waveform type
     * @param frequency Waveform frequency in Hz
     * @param amplitude Waveform amplitude in volts
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_waveform(
        WaveformType waveform_type,
        float frequency,
        float amplitude
    ) noexcept;

    /**
     * @brief Get waveform parameters
     * @param waveform_type Pointer to store waveform type
     * @param frequency Pointer to store frequency
     * @param amplitude Pointer to store amplitude
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_waveform(
        WaveformType* waveform_type,
        float* frequency,
        float* amplitude
    ) const noexcept;

    /**
     * @brief Start waveform generation (DMA circular mode)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> start_waveform() noexcept;

    /**
     * @brief Stop waveform generation
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> stop_waveform() noexcept;

    /**
     * @brief Get comprehensive DAC status for TUI
     * @param status Reference to DACStatus message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_status(DACStatus& status) const noexcept;

    /**
     * @brief Get DAC measurements for TUI
     * @param measurements Reference to DACMeasurements message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_measurements(DACMeasurements& measurements) const noexcept;

    /**
     * @brief Get full DAC configuration
     * @param config Reference to DACConfig message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_config(DACConfig& config) const noexcept;

    /**
     * @brief Set DAC configuration
     * @param config DAC configuration to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_config(const DACConfig& config) noexcept;

    /**
     * @brief Set DAC configuration from request
     * @param config DAC configuration request to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_config(const DACConfigRequest& config) noexcept;

    /**
     * @brief Check if DAC has new data available
     * @return true if updates available, false otherwise
     */
    bool has_updates() const noexcept;

    /**
     * @brief Wait for DAC update with timeout
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
     * @brief Check if DAC controller is initialized
     * @return bool - true if initialized (no error possible)
     */
    bool is_initialized() const noexcept;

    /**
     * @brief Check if error condition exists
     * @return bool - true if error present (no error possible)
     */
    bool has_error() const noexcept;

    /**
     * @brief Check if waveform error exists
     * @return bool - true if waveform error present (no error possible)
     */
    bool has_waveform_error() const noexcept;
};

#endif
