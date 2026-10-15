#ifndef __DAC_HPP__
#define __DAC_HPP__

#include "board.h"
#include "error.hpp"
#include "mutex.hpp"
#include "sem.hpp"
#include "protocol.pb.h"
#include <cstdint>
#include <expected>
#include <mutex>

/**
 * @brief DAC driver wrapper with waveform generation support
 *
 * Wraps STM32 HAL DAC and provides both low-level control and high-level
 * waveform generation using DMA. Supports constant voltage mode and
 * waveform generation mode with pre-generated sample buffers.
 */
class DacDriver {
    DAC_HandleTypeDef* dac_handle_;
    uint32_t channel_;
    
    // Thread safety
    mutable os::mutex mutex_;
    
    // Cached state for fast TUI updates
    mutable DACStatus cached_status_;
    mutable DACMeasurements cached_measurements_;
    mutable uint32_t last_update_ms_;
    
    // Current configuration
    DACMode current_mode_;
    float current_voltage_;
    WaveformType current_waveform_type_;
    float current_frequency_;
    float current_amplitude_;

    // DMA waveform buffer - HAL expects uint32_t even for 12-bit DAC
    static constexpr size_t MAX_WAVEFORM_SAMPLES = 256;
    uint32_t waveform_buffer_[MAX_WAVEFORM_SAMPLES];
    size_t waveform_buffer_size_;
    bool waveform_buffer_ready_;
    
    // Update signaling
    os::semaphore update_sem_;

public:
    /**
     * @brief Construct DAC driver for a single channel
     * @param channel_config DAC_channel_t with DAC handle and channel number
     */
    explicit DacDriver(const DAC_channel_t& channel_config);

    /**
     * @brief Destroy DAC driver
     */
    ~DacDriver();

    /**
     * @brief Initialize the DAC driver
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Enable the DAC channel
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> enable() noexcept;

    /**
     * @brief Disable the DAC channel
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> disable() noexcept;

    /**
     * @brief Set output voltage for the DAC channel
     * @param voltage Output voltage in volts (0-3.3V)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_voltage(float voltage) noexcept;

    /**
     * @brief Set waveform type for the DAC channel
     * @param waveform_type Waveform type (sine, square, triangle, etc.)
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
     * @brief Start waveform generation using DMA circular mode
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> start_waveform() noexcept;

    /**
     * @brief Stop waveform generation
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> stop_waveform() noexcept;

    /**
     * @brief Check if the DAC channel is enabled
     * @return std::expected<bool, ControllerError> - true if enabled, error on failure
     */
    std::expected<bool, ControllerError> is_enabled() const noexcept;

    /**
     * @brief Get the DAC handle
     * @return DAC_HandleTypeDef* - handle pointer
     */
    DAC_HandleTypeDef* get_handle() const noexcept;

    /**
     * @brief Get the channel number
     * @return uint32_t - channel number (DAC_CHANNEL_1, DAC_CHANNEL_2, etc.)
     */
    uint32_t get_channel() const noexcept;

    // ==================== High-level Controller Methods ====================

    /**
     * @brief Enable/disable DAC channels
     * @param enable true to enable, false to disable
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_enabled(bool enable) noexcept;

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
     * @brief Get output voltage
     * @return std::expected<float, ControllerError> - output voltage in volts, error on failure
     */
    std::expected<float, ControllerError> get_voltage() const noexcept;

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
     * @brief Get full DAC state
     * @param state Reference to DACState message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_state(DACState& state) const noexcept;

    /**
     * @brief Populate SystemStatus with DAC status
     * @param status Reference to SystemStatus message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_status(SystemStatus& status) const noexcept;

    /**
     * @brief Populate SystemStatus with DAC measurements
     * @param status Reference to SystemStatus message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_measurements(SystemStatus& status) const noexcept;

    /**
     * @brief Populate SystemStatus with DAC state
     * @param status Reference to SystemStatus message to populate
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> get_state(SystemStatus& status) const noexcept;

    /**
     * @brief Set DAC state
     * @param state DAC state to apply
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_state(const DACState& state) noexcept;

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

private:
    /**
     * @brief Generate waveform samples in buffer
     * @param waveform_type Waveform type
     * @param frequency Waveform frequency in Hz (for scaling)
     * @param amplitude Waveform amplitude in volts
     * @param num_samples Number of samples to generate (max MAX_WAVEFORM_SAMPLES)
     * @return Number of samples generated, 0 on error
     */
    size_t generate_waveform_samples(
        WaveformType waveform_type,
        float frequency,
        float amplitude,
        size_t num_samples
    ) noexcept;

    /**
     * @brief Configure DMA for waveform generation
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> configure_dma_waveform() noexcept;

    /**
     * @brief Stop DMA waveform generation
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> stop_dma_waveform() noexcept;
};

#endif
