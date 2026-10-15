#ifndef __OSCILLOSCOPE_HPP__
#define __OSCILLOSCOPE_HPP__

#include "board.h"
#include "error.hpp"
#include "mutex.hpp"
#include <array>
#include <cstdint>
#include <vector>
#include <expected>
#include <memory>

/**
 * @brief Oscilloscope component for multi-channel ADC sampling
 *
 * Provides DMA-based ADC sampling triggered by TIM8.
 * Supports up to 5 ADCs (ADC1-ADC5) with configurable sample counts per ADC.
 * Multiple channels can be sampled per ADC, with samples distributed equally.
 *
 * Buffer layout:
 * - Single 4KB DMA buffer partitioned across all ADCs
 * - Each ADC gets a contiguous partition based on its sample_count
 * - Channels on same ADC share their partition equally
 *
 * Timer Trigger:
 * - Uses g_board_tim_adc_trgo (TIM8) for ADC trigger via TRGO
 * - Timer must be configured in CubeMX with appropriate period and OC mode
 * - Oscilloscope starts/stops the timer during arm()/stop()
 */
class Oscilloscope {
public:
    Oscilloscope();
    ~Oscilloscope();

    /**
     * @brief Initialize the oscilloscope component
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> initialize() noexcept;

    /**
     * @brief Configure an ADC with sample count and trigger settings
     * @param adc ADC handle (hadc1, hadc2, hadc3, etc.)
     * @param sample_count Total samples for this ADC (max 2048 per ADC, total across all ADCs ≤ 2048)
     * @param trigger_source TIM8 trigger source (ADC_EXTERNALTRIG_T8_TRGO, etc.)
     * @param trigger_edge Trigger edge (ADC_EXTERNALTRIGCONVEDGE_RISING, etc.)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> configure_adc(
        ADC_HandleTypeDef* adc,
        uint32_t sample_count,
        uint32_t trigger_source,
        uint32_t trigger_edge
    ) noexcept;

    /**
     * @brief Add a channel to an already configured ADC
     * @param adc ADC handle (must be configured first via configure_adc)
     * @param channel ADC channel number (0-18)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> add_channel(
        ADC_HandleTypeDef* adc,
        uint32_t channel
    ) noexcept;

    /**
     * @brief Validate current configuration
     * @return std::expected<void, ControllerError> - success if valid, error on failure
     */
    std::expected<void, ControllerError> validate_config() const noexcept;

    /**
     * @brief Get remaining available samples across all ADCs
     * @return Number of samples still available for allocation
     */
    uint32_t get_remaining_samples() const noexcept;

    /**
     * @brief Arm the oscilloscope for acquisition
     * Prepares all configured ADCs and DMA for triggering.
     * Must be called before start().
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> arm() noexcept;

    /**
     * @brief Start acquisition - waits for TIM8 trigger
     * Starts DMA transfers and waits for the trigger event.
     * Returns when all samples are collected.
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> start() noexcept;

    /**
     * @brief Stop acquisition
     * Stops all DMA transfers and ADC conversions.
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> stop() noexcept;

    /**
     * @brief Check if data is ready (acquisition complete)
     * @return true if data is ready, false otherwise
     */
    bool is_data_ready() const noexcept;

    /**
     * @brief Get samples for a specific channel
     * @param channel_index Channel index (0-based, order of add_channel calls)
     * @param buffer Buffer to store samples
     * @param max_size Maximum number of samples to copy
     * @return Number of samples copied, or error
     */
    std::expected<uint32_t, ControllerError> get_samples(
        uint32_t channel_index,
        uint16_t* buffer,
        uint32_t max_size
    ) const noexcept;

    /**
     * @brief Get direct pointer to channel's DMA buffer
     * @param channel_index Channel index
     * @return Pointer to channel's buffer, or nullptr if invalid
     */
    const uint16_t* get_channel_buffer(uint32_t channel_index) const noexcept;

    /**
     * @brief Get sample count for a specific channel
     * @param channel_index Channel index
     * @return Number of samples allocated to this channel
     */
    uint32_t get_channel_sample_count(uint32_t channel_index) const noexcept;

    /**
     * @brief Get total number of configured channels
     * @return Number of channels
     */
    uint32_t get_num_channels() const noexcept;

    /**
     * @brief Check if oscilloscope is armed
     * @return true if armed, false otherwise
     */
    bool is_armed() const noexcept;

    /**
     * @brief Check if oscilloscope is initialized
     * @return true if initialized, false otherwise
     */
    bool is_initialized() const noexcept;

private:
    // Board ADC handles (ADC1-ADC5)
    static constexpr size_t MAX_ADC_COUNT = 5;
    std::array<ADC_HandleTypeDef*, MAX_ADC_COUNT> adc_handles_;

    // Single 4KB buffer partitioned across channels
    std::unique_ptr<uint16_t[], void(*)(void*)> dma_buffer_;
    size_t buffer_size_samples_;  // 2048

    // Per-ADC configuration
    struct ADCConfig {
        ADC_HandleTypeDef* adc;
        uint32_t sample_count;
        uint32_t trigger_source;
        uint32_t trigger_edge;
        std::vector<uint32_t> channels;  // Channel numbers
        uint32_t samples_per_channel;    // sample_count / num_channels
    };
    std::vector<ADCConfig> adc_configs_;

    // Channel info for data access
    struct ChannelInfo {
        uint32_t adc_index;           // Index into adc_configs_
        uint32_t channel;             // Channel number
        uint16_t* buffer_ptr;         // Partition pointer
        uint32_t sample_count;        // Samples for this channel
    };
    std::vector<ChannelInfo> channel_infos_;

    // Acquisition state
    bool initialized_;
    bool armed_;
    bool data_ready_;
    uint32_t num_samples_acquired_;

    // Thread safety - must be last to avoid initialization order issues
    mutable os::mutex mutex_;

    // Helper methods
    ADCConfig* find_adc_config(ADC_HandleTypeDef* adc);
    const ADCConfig* find_adc_config(ADC_HandleTypeDef* adc) const;
    std::expected<void, ControllerError> configure_adc_hardware(ADCConfig& config) noexcept;
    std::expected<void, ControllerError> start_adc_dma(ADCConfig& config) noexcept;
    std::expected<void, ControllerError> stop_adc_dma(ADCConfig& config) noexcept;
};

#endif
