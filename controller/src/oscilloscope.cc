#include "oscilloscope.hpp"
#include "log.hpp"
#include "board.h"
#include <cstring>
#include <new>
#include <mutex>

// DMA buffer size: 4KB = 2048 samples @ 2 bytes each
constexpr size_t DMA_BUFFER_SIZE_SAMPLES = 2048;
constexpr size_t DMA_BUFFER_SIZE_BYTES = DMA_BUFFER_SIZE_SAMPLES * sizeof(uint16_t);
constexpr size_t DMA_BUFFER_ALIGNMENT = 32;  // 32-byte alignment for DMA

// DMA request numbers for each ADC
// Note: Actual DMA streams/channels must be configured in CubeMX
// Note: These values are for reference - the actual DMA configuration is done via HAL

// =============================================================================
// Constructor / Destructor
// =============================================================================

Oscilloscope::Oscilloscope()
    : adc_handles_{nullptr, nullptr, nullptr, nullptr, nullptr},
      dma_buffer_{nullptr, free},
      buffer_size_samples_{DMA_BUFFER_SIZE_SAMPLES},
      initialized_{false},
      armed_{false},
      data_ready_{false},
      num_samples_acquired_{0}
    , mutex_(nullptr)
{
    // Get board ADC handles from main.c
    extern ADC_HandleTypeDef hadc1;
    extern ADC_HandleTypeDef hadc2;
    extern ADC_HandleTypeDef hadc3;
    extern ADC_HandleTypeDef hadc4;
    extern ADC_HandleTypeDef hadc5;
    
    adc_handles_[0] = &hadc1;
    adc_handles_[1] = &hadc2;
    adc_handles_[2] = &hadc3;
    adc_handles_[3] = &hadc4;
    adc_handles_[4] = &hadc5;

    // Allocate aligned DMA buffer
    void* buffer = aligned_alloc(DMA_BUFFER_ALIGNMENT, DMA_BUFFER_SIZE_BYTES);
    if (!buffer) {
        os::error("Failed to allocate DMA buffer\n");
        return;
    }

    // Create unique_ptr with custom deleter
    dma_buffer_.reset(static_cast<uint16_t*>(buffer));
    std::memset(dma_buffer_.get(), 0, DMA_BUFFER_SIZE_BYTES);

    initialized_ = true;
}

Oscilloscope::~Oscilloscope() {
    // Stop any ongoing acquisition
    stop();
}

// =============================================================================
// Public Methods
// =============================================================================

std::expected<void, ControllerError> Oscilloscope::initialize() noexcept {
    if (!initialized_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Validate no duplicate ADCs in config
    for (size_t i = 0; i < adc_configs_.size(); ++i) {
        for (size_t j = i + 1; j < adc_configs_.size(); ++j) {
            if (adc_configs_[i].adc == adc_configs_[j].adc) {
                os::error("Duplicate ADC configuration: %p\n", adc_configs_[i].adc);
                return std::unexpected(ControllerError::UnknownError);
            }
        }
    }

    return {};
}

std::expected<void, ControllerError> Oscilloscope::configure_adc(
    ADC_HandleTypeDef* adc,
    uint32_t sample_count,
    uint32_t trigger_source,
    uint32_t trigger_edge
) noexcept {
    if (!initialized_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Validate ADC handle
    if (!adc) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Validate sample count
    if (sample_count == 0 || sample_count > DMA_BUFFER_SIZE_SAMPLES) {
        os::error("Invalid sample count: %u (max: %zu)\n", sample_count, DMA_BUFFER_SIZE_SAMPLES);
        return std::unexpected(ControllerError::UnknownError);
    }

    // Check if ADC already configured
    if (find_adc_config(adc)) {
        os::error("ADC already configured: %p\n", adc);
        return std::unexpected(ControllerError::UnknownError);
    }

    // Validate total buffer allocation
    uint32_t total_samples = sample_count;
    for (const auto& cfg : adc_configs_) {
        total_samples += cfg.sample_count;
    }
    if (total_samples > DMA_BUFFER_SIZE_SAMPLES) {
        os::error("Total sample count exceeds buffer: %u > %zu\n", 
                  total_samples, DMA_BUFFER_SIZE_SAMPLES);
        return std::unexpected(ControllerError::UnknownError);
    }

    // Create new ADC config
    ADCConfig config;
    config.adc = adc;
    config.sample_count = sample_count;
    config.trigger_source = trigger_source;
    config.trigger_edge = trigger_edge;
    config.channels.clear();
    config.samples_per_channel = 0;

    adc_configs_.push_back(std::move(config));

    return {};
}

std::expected<void, ControllerError> Oscilloscope::add_channel(
    ADC_HandleTypeDef* adc,
    uint32_t channel
) noexcept {
    if (!initialized_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Find ADC config
    ADCConfig* config = find_adc_config(adc);
    if (!config) {
        os::error("ADC not configured: %p\n", adc);
        return std::unexpected(ControllerError::UnknownError);
    }

    // Validate channel number
    if (channel > 18) {
        os::error("Invalid channel number: %u\n", channel);
        return std::unexpected(ControllerError::UnknownError);
    }

    // Check for duplicate channel
    for (uint32_t ch : config->channels) {
        if (ch == channel) {
            os::error("Channel already added: %u\n", channel);
            return std::unexpected(ControllerError::UnknownError);
        }
    }

    // Add channel
    config->channels.push_back(channel);

    // Recalculate samples per channel (equal distribution)
    config->samples_per_channel = config->sample_count / config->channels.size();

    return {};
}

std::expected<void, ControllerError> Oscilloscope::validate_config() const noexcept {
    if (!initialized_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    if (adc_configs_.empty()) {
        os::error("No ADCs configured\n");
        return std::unexpected(ControllerError::UnknownError);
    }

    // Calculate total samples
    uint32_t total_samples = 0;
    for (const auto& cfg : adc_configs_) {
        total_samples += cfg.sample_count;

        if (cfg.channels.empty()) {
            os::error("ADC %p has no channels configured\n", cfg.adc);
            return std::unexpected(ControllerError::UnknownError);
        }

        if (cfg.samples_per_channel == 0) {
            os::error("ADC %p has 0 samples per channel\n", cfg.adc);
            return std::unexpected(ControllerError::UnknownError);
        }
    }

    if (total_samples > DMA_BUFFER_SIZE_SAMPLES) {
        os::error("Total samples exceed buffer: %u > %zu\n", 
                  total_samples, DMA_BUFFER_SIZE_SAMPLES);
        return std::unexpected(ControllerError::UnknownError);
    }

    return {};
}

uint32_t Oscilloscope::get_remaining_samples() const noexcept {
    uint32_t total = 0;
    for (const auto& cfg : adc_configs_) {
        total += cfg.sample_count;
    }
    return DMA_BUFFER_SIZE_SAMPLES - total;
}

std::expected<void, ControllerError> Oscilloscope::arm() noexcept {
    std::lock_guard lock(mutex_);

    if (!initialized_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Validate configuration
    auto result = validate_config();
    if (!result) {
        return result;
    }

    // Build channel info for data access
    channel_infos_.clear();
    uint16_t* buffer_ptr = dma_buffer_.get();
    
    for (size_t adc_idx = 0; adc_idx < adc_configs_.size(); ++adc_idx) {
        const auto& cfg = adc_configs_[adc_idx];
        
        for (uint32_t channel : cfg.channels) {
            ChannelInfo info;
            info.adc_index = static_cast<uint32_t>(adc_idx);
            info.channel = channel;
            info.buffer_ptr = buffer_ptr;
            info.sample_count = cfg.samples_per_channel;
            channel_infos_.push_back(info);

            buffer_ptr += cfg.samples_per_channel;
        }
    }

    // Configure each ADC
    for (auto& cfg : adc_configs_) {
        result = configure_adc_hardware(cfg);
        if (!result) {
            os::error("Failed to configure ADC %p\n", cfg.adc);
            return result;
        }
    }

    armed_ = true;
    data_ready_ = false;
    num_samples_acquired_ = 0;

    return {};
}

std::expected<void, ControllerError> Oscilloscope::start() noexcept {
    std::lock_guard lock(mutex_);

    if (!initialized_ || !armed_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Start DMA for each ADC
    for (auto& cfg : adc_configs_) {
        auto result = start_adc_dma(cfg);
        if (!result) {
            os::error("Failed to start DMA for ADC %p\n", cfg.adc);
            stop_adc_dma(cfg);
            return result;
        }
    }

    // Wait for all acquisitions to complete
    // This is a blocking call - in production, use interrupts or DMA callbacks
    uint32_t timeout_ms = 1000;  // 1 second timeout
    uint32_t start_ms = os::timestamp_ms();

    while (true) {
        bool all_done = true;
        for (const auto& cfg : adc_configs_) {
            uint32_t state = HAL_ADC_GetState(cfg.adc);
            if ((state & HAL_ADC_STATE_REG_BUSY) != 0) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            break;
        }

        if (os::timestamp_ms() - start_ms > timeout_ms) {
            os::error("Acquisition timeout\n");
            stop_locked();  // mutex_ already held; stop() would deadlock
            return std::unexpected(ControllerError::HAL_TIMEOUT);
        }

        os::delay(1);
    }

    num_samples_acquired_ = DMA_BUFFER_SIZE_SAMPLES;
    data_ready_ = true;
    armed_ = false;

    return {};
}

std::expected<void, ControllerError> Oscilloscope::stop() noexcept {
    std::lock_guard lock(mutex_);
    return stop_locked();
}

std::expected<void, ControllerError> Oscilloscope::stop_locked() noexcept {
    if (!initialized_) {
        return std::unexpected(ControllerError::UnknownError);
    }

    // Stop DMA for each ADC
    for (auto& cfg : adc_configs_) {
        stop_adc_dma(cfg);
    }

    armed_ = false;
    data_ready_ = false;
    num_samples_acquired_ = 0;

    return {};
}

bool Oscilloscope::is_data_ready() const noexcept {
    return data_ready_;
}

std::expected<uint32_t, ControllerError> Oscilloscope::get_samples(
    uint32_t channel_index,
    uint16_t* buffer,
    uint32_t max_size
) const noexcept {
    if (!buffer) {
        return std::unexpected(ControllerError::UnknownError);
    }

    if (channel_index >= channel_infos_.size()) {
        os::error("Invalid channel index: %u (max: %zu)\n", 
                  channel_index, channel_infos_.size());
        return std::unexpected(ControllerError::UnknownError);
    }

    if (!data_ready_) {
        os::error("Data not ready - call start() first\n");
        return std::unexpected(ControllerError::UnknownError);
    }

    const auto& info = channel_infos_[channel_index];
    uint32_t copy_count = std::min(info.sample_count, max_size);

    std::memcpy(buffer, info.buffer_ptr, copy_count * sizeof(uint16_t));

    return copy_count;
}

const uint16_t* Oscilloscope::get_channel_buffer(uint32_t channel_index) const noexcept {
    if (channel_index >= channel_infos_.size()) {
        return nullptr;
    }

    return channel_infos_[channel_index].buffer_ptr;
}

uint32_t Oscilloscope::get_channel_sample_count(uint32_t channel_index) const noexcept {
    if (channel_index >= channel_infos_.size()) {
        return 0;
    }

    return channel_infos_[channel_index].sample_count;
}

uint32_t Oscilloscope::get_num_channels() const noexcept {
    return static_cast<uint32_t>(channel_infos_.size());
}

bool Oscilloscope::is_armed() const noexcept {
    return armed_;
}

bool Oscilloscope::is_initialized() const noexcept {
    return initialized_;
}

// =============================================================================
// Private Methods
// =============================================================================

Oscilloscope::ADCConfig* Oscilloscope::find_adc_config(ADC_HandleTypeDef* adc) {
    for (auto& cfg : adc_configs_) {
        if (cfg.adc == adc) {
            return &cfg;
        }
    }
    return nullptr;
}

const Oscilloscope::ADCConfig* Oscilloscope::find_adc_config(ADC_HandleTypeDef* adc) const {
    for (const auto& cfg : adc_configs_) {
        if (cfg.adc == adc) {
            return &cfg;
        }
    }
    return nullptr;
}

std::expected<void, ControllerError> Oscilloscope::configure_adc_hardware(ADCConfig& config) noexcept {
    // Just configure the channels - base ADC configuration is done by CubeMX
    ADC_ChannelConfTypeDef sConfig = {};
    sConfig.SamplingTime = ADC_SAMPLETIME_640CYCLES_5;

    for (uint32_t i = 0; i < config.channels.size(); ++i) {
        sConfig.Channel = config.channels[i];
        sConfig.Rank = static_cast<uint32_t>(i + 1);  // Rank 1, 2, 3, ...

        HAL_StatusTypeDef status = HAL_ADC_ConfigChannel(config.adc, &sConfig);
        if (status != HAL_OK) {
            os::error("HAL_ADC_ConfigChannel failed for channel %u: %d\n",
                      config.channels[i], static_cast<int>(status));
            return std::unexpected(from_hal_status(status));
        }
    }

    return {};
}

std::expected<void, ControllerError> Oscilloscope::start_adc_dma(ADCConfig& config) noexcept {
    // Find the buffer partition for this ADC
    uint16_t* adc_buffer_ptr = nullptr;
    uint32_t adc_sample_count = 0;
    
    uint16_t* buffer_iter = dma_buffer_.get();
    for (const auto& cfg : adc_configs_) {
        if (cfg.adc == config.adc) {
            adc_buffer_ptr = buffer_iter;
            adc_sample_count = cfg.sample_count;
            break;
        }
        buffer_iter += cfg.sample_count;
    }
    
    if (!adc_buffer_ptr || adc_sample_count == 0) {
        os::error("Failed to find buffer partition for ADC %p\n", config.adc);
        return std::unexpected(ControllerError::UnknownError);
    }

    // Start ADC with DMA
    // HAL_ADC_Start_DMA() expects a uint32_t* buffer, but our ADC values are 16-bit
    // The DMA will transfer 16-bit values, but the buffer pointer must be cast
    HAL_StatusTypeDef status = HAL_ADC_Start_DMA(config.adc, 
                                                  reinterpret_cast<uint32_t*>(adc_buffer_ptr), 
                                                  adc_sample_count);
    if (status != HAL_OK) {
        os::error("HAL_ADC_Start_DMA failed: %d\n", static_cast<int>(status));
        return std::unexpected(from_hal_status(status));
    }

    return {};
}

std::expected<void, ControllerError> Oscilloscope::stop_adc_dma(ADCConfig& config) noexcept {
    HAL_ADC_Stop(config.adc);
    return {};
}
