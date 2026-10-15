#include "dac.hpp"
#include "log.hpp"
#include "dac_waveform.hpp"
#include <cstring>

// =============================================================================
// Constants
// =============================================================================

// DAC reference voltage for waveform generation
constexpr float DAC_REFERENCE_VOLTAGE = 3.3f;

// =============================================================================
// Constructor/Destructor
// =============================================================================

DacDriver::DacDriver(const DAC_channel_t& channel_config)
    : dac_handle_(channel_config.DACx),
      channel_(channel_config.channel),
      mutex_(nullptr),
      update_sem_(1, 0, nullptr)
{
    // Initialize cached values
    cached_status_ = DACStatus_init_default;
    cached_measurements_ = DACMeasurements_init_default;
    last_update_ms_ = 0;
    current_mode_ = DACMode_DAC_MODE_VOLTAGE;
    current_voltage_ = 0.0f;
    current_waveform_type_ = WaveformType_WAVEFORM_SINE;
    current_frequency_ = 0.0f;
    current_amplitude_ = 0.0f;
    waveform_buffer_size_ = 0;
    waveform_buffer_ready_ = false;
}

DacDriver::~DacDriver() = default;

// =============================================================================
// Initialization
// =============================================================================

std::expected<void, ControllerError> DacDriver::initialize() noexcept {
    if (dac_handle_ == nullptr) {
        os::error("DAC handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Initialize cached values with defaults
    cached_status_.enabled = false;
    cached_status_.mode = DACMode_DAC_MODE_VOLTAGE;
    cached_status_.waveform_type = WaveformType_WAVEFORM_SINE;
    cached_status_.frequency = 0.0f;
    cached_status_.amplitude = 0.0f;
    cached_status_.error = false;
    cached_status_.waveform_error = false;
    cached_status_.timestamp_ms = 0;
    
    cached_measurements_.voltage_output = 0.0f;
    
    return {};
}

// =============================================================================
// Basic Control
// =============================================================================

std::expected<void, ControllerError> DacDriver::enable() noexcept {
    std::lock_guard lock(mutex_);
    
    if (dac_handle_ == nullptr) {
        os::error("DAC handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    HAL_StatusTypeDef result = HAL_DAC_Start(dac_handle_, channel_);

    if (result != HAL_OK) {
        os::error("Failed to enable DAC: HAL error %d\n", result);
        return std::unexpected(from_hal_status(result));
    }

    cached_status_.enabled = true;
    trigger_update();
    return {};
}

std::expected<void, ControllerError> DacDriver::disable() noexcept {
    std::lock_guard lock(mutex_);
    
    if (dac_handle_ == nullptr) {
        os::error("DAC handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    HAL_StatusTypeDef result = HAL_DAC_Stop(dac_handle_, channel_);

    if (result != HAL_OK) {
        os::error("Failed to disable DAC: HAL error %d\n", result);
        return std::unexpected(from_hal_status(result));
    }

    // Stop any active DMA waveform
    stop_dma_waveform();
    
    cached_status_.enabled = false;
    trigger_update();
    return {};
}

// =============================================================================
// Voltage Control
// =============================================================================

std::expected<void, ControllerError> DacDriver::set_voltage(float voltage) noexcept {
    std::lock_guard lock(mutex_);
    
    if (dac_handle_ == nullptr) {
        os::error("DAC handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Clamp voltage to valid range (0-3.3V)
    if (voltage < 0.0f) voltage = 0.0f;
    if (voltage > 3.3f) voltage = 3.3f;

    // Convert voltage to DAC code (12-bit)
    uint32_t dac_code = static_cast<uint32_t>((voltage / 3.3f) * 4095 + 0.5f);

    HAL_StatusTypeDef result = HAL_DAC_SetValue(
        dac_handle_,
        channel_,
        DAC_ALIGN_12B_R,
        dac_code
    );

    if (result != HAL_OK) {
        os::error("Failed to set DAC voltage: HAL error %d\n", result);
        return std::unexpected(from_hal_status(result));
    }

    current_voltage_ = voltage;
    cached_measurements_.voltage_output = voltage;
    
    // If we're in waveform mode, stop waveform and switch to voltage mode
    if (current_mode_ == DACMode_DAC_MODE_WAVEFORM) {
        stop_dma_waveform();
        current_mode_ = DACMode_DAC_MODE_VOLTAGE;
    }
    
    trigger_update();
    return {};
}

// =============================================================================
// Waveform Configuration
// =============================================================================

std::expected<void, ControllerError> DacDriver::set_waveform(
    WaveformType waveform_type,
    float frequency,
    float amplitude
) noexcept {
    std::lock_guard lock(mutex_);
    
    if (dac_handle_ == nullptr) {
        os::error("DAC handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Validate parameters
    if (frequency <= 0.0f || frequency > 100000.0f) {  // 0.1Hz - 100kHz
        os::error("Invalid waveform frequency: %.2f Hz\n", frequency);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (amplitude <= 0.0f || amplitude > 3.3f) {
        os::error("Invalid waveform amplitude: %.2f V\n", amplitude);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    // Store waveform configuration
    current_waveform_type_ = waveform_type;
    current_frequency_ = frequency;
    current_amplitude_ = amplitude;

    // Generate waveform samples in buffer
    // Use 256 samples for good frequency resolution
    waveform_buffer_size_ = generate_waveform_samples(
        waveform_type,
        frequency,
        amplitude,
        MAX_WAVEFORM_SAMPLES
    );
    
    if (waveform_buffer_size_ == 0) {
        os::error("Failed to generate waveform samples\n");
        return std::unexpected(ControllerError::DAC_WAVEFORM_ERROR);
    }
    
    waveform_buffer_ready_ = true;
    
    // If in voltage mode, switch to waveform mode
    if (current_mode_ == DACMode_DAC_MODE_VOLTAGE) {
        current_mode_ = DACMode_DAC_MODE_WAVEFORM;
    }
    
    trigger_update();
    return {};
}

// =============================================================================
// Waveform Generation (DMA)
// =============================================================================

std::expected<void, ControllerError> DacDriver::start_waveform() noexcept {
    std::lock_guard lock(mutex_);
    
    if (!waveform_buffer_ready_) {
        os::error("Waveform buffer not ready - call set_waveform() first\n");
        return std::unexpected(ControllerError::DAC_WAVEFORM_ERROR);
    }

    if (dac_handle_ == nullptr) {
        os::error("DAC handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Configure DMA for waveform generation
    auto dma_result = configure_dma_waveform();
    if (!dma_result) {
        os::error("Failed to configure DMA: %d\n", static_cast<int>(dma_result.error()));
        return std::unexpected(dma_result.error());
    }

    // Start DAC with DMA circular mode
    HAL_StatusTypeDef result = HAL_DAC_Start_DMA(
        dac_handle_,
        channel_,
        waveform_buffer_,
        waveform_buffer_size_,
        DAC_ALIGN_12B_R
    );

    if (result != HAL_OK) {
        os::error("Failed to start DAC DMA: HAL error %d\n", result);
        return std::unexpected(from_hal_status(result));
    }

    cached_status_.enabled = true;
    trigger_update();
    return {};
}

std::expected<void, ControllerError> DacDriver::stop_waveform() noexcept {
    std::lock_guard lock(mutex_);
    
    auto result = stop_dma_waveform();
    if (!result) {
        return std::unexpected(result.error());
    }
    
    cached_status_.enabled = false;
    trigger_update();
    return {};
}

std::expected<void, ControllerError> DacDriver::stop_dma_waveform() noexcept {
    if (dac_handle_ == nullptr) {
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    HAL_StatusTypeDef result = HAL_DAC_Stop_DMA(dac_handle_, channel_);

    if (result != HAL_OK) {
        os::error("Failed to stop DAC DMA: HAL error %d\n", result);
        return std::unexpected(from_hal_status(result));
    }

    return {};
}

std::expected<void, ControllerError> DacDriver::configure_dma_waveform() noexcept {
    // This is a placeholder - actual DMA configuration should be done via CubeMX
    // The user needs to configure:
    // 1. DMA channel for DAC (DMA1_Channel3 for DAC1_CH1, DMA1_Channel4 for DAC1_CH2, etc.)
    // 2. Circular mode enabled
    // 3. Memory-to-peripheral transfer
    // 4. Proper buffer size
    
    // For now, we just verify the DMA is already configured
    // The HAL_DAC_Start_DMA will use the pre-configured DMA settings
    
    return {};
}

// =============================================================================
// Helper Methods
// =============================================================================

size_t DacDriver::generate_waveform_samples(
    WaveformType waveform_type,
    [[maybe_unused]] float frequency,
    float amplitude,
    size_t num_samples
) noexcept {
    // waveform_buffer_ is a member array, never NULL
    if (num_samples == 0 || num_samples > MAX_WAVEFORM_SAMPLES) {
        return 0;
    }

    // Offset for bipolar waveforms (center at 1.65V)
    float offset = 1.65f;
    
    switch (waveform_type) {
        case WaveformType_WAVEFORM_SINE:
            return generate_sine_wave(
                waveform_buffer_,
                num_samples,
                amplitude,
                offset
            );
            
        case WaveformType_WAVEFORM_SQUARE:
            return generate_square_wave(
                waveform_buffer_,
                num_samples,
                amplitude,
                offset
            );
            
        case WaveformType_WAVEFORM_TRIANGLE:
            return generate_triangle_wave(
                waveform_buffer_,
                num_samples,
                amplitude,
                offset
            );
            
        case WaveformType_WAVEFORM_SAWTOOTH:
            return generate_sawtooth_wave(
                waveform_buffer_,
                num_samples,
                amplitude,
                offset
            );
            
        case WaveformType_WAVEFORM_CUSTOM:
        default:
            os::error("Custom waveform not supported\n");
            return 0;
    }
}

// =============================================================================
// Getters
// =============================================================================

std::expected<bool, ControllerError> DacDriver::is_enabled() const noexcept {
    std::lock_guard lock(mutex_);
    
    if (dac_handle_ == nullptr) {
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Check DAC state
    bool enabled = (cached_status_.enabled == true);
    return enabled;
}

DAC_HandleTypeDef* DacDriver::get_handle() const noexcept {
    return dac_handle_;
}

uint32_t DacDriver::get_channel() const noexcept {
    return channel_;
}

// =============================================================================
// High-level Controller Methods
// =============================================================================

std::expected<void, ControllerError> DacDriver::set_enabled(bool enable_dac) noexcept {
    if (enable_dac) {
        return enable();
    } else {
        return disable();
    }
}

DACMode DacDriver::get_mode() const noexcept {
    std::lock_guard lock(mutex_);
    return current_mode_;
}

std::expected<void, ControllerError> DacDriver::set_mode(DACMode mode) noexcept {
    std::lock_guard lock(mutex_);
    
    // Stop waveform if switching from waveform mode
    if (current_mode_ == DACMode_DAC_MODE_WAVEFORM && mode == DACMode_DAC_MODE_VOLTAGE) {
        auto result = stop_dma_waveform();
        if (!result) {
            return std::unexpected(result.error());
        }
    }
    
    current_mode_ = mode;
    trigger_update();
    return {};
}

std::expected<float, ControllerError> DacDriver::get_voltage() const noexcept {
    std::lock_guard lock(mutex_);
    return current_voltage_;
}

std::expected<void, ControllerError> DacDriver::get_waveform(
    WaveformType* waveform_type,
    float* frequency,
    float* amplitude
) const noexcept {
    std::lock_guard lock(mutex_);
    
    if (!waveform_type || !frequency || !amplitude) {
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }
    
    *waveform_type = current_waveform_type_;
    *frequency = current_frequency_;
    *amplitude = current_amplitude_;
    
    return {};
}

// =============================================================================
// Protocol Buffer Methods
// =============================================================================

std::expected<void, ControllerError> DacDriver::get_status(DACStatus& status) const noexcept {
    std::lock_guard lock(mutex_);
    
    status.enabled = cached_status_.enabled;
    status.mode = cached_status_.mode;
    status.waveform_type = cached_status_.waveform_type;
    status.frequency = cached_status_.frequency;
    status.amplitude = cached_status_.amplitude;
    status.error = cached_status_.error;
    status.waveform_error = cached_status_.waveform_error;
    status.timestamp_ms = cached_status_.timestamp_ms;
    
    return {};
}

std::expected<void, ControllerError> DacDriver::get_measurements(DACMeasurements& measurements) const noexcept {
    std::lock_guard lock(mutex_);
    
    measurements.voltage_output = cached_measurements_.voltage_output;
    
    return {};
}

std::expected<void, ControllerError> DacDriver::get_state(DACState& state) const noexcept {
    std::lock_guard lock(mutex_);

    state.voltage = current_voltage_;
    state.waveform_type = current_waveform_type_;
    state.frequency = current_frequency_;
    state.amplitude = current_amplitude_;

    return {};
}

std::expected<void, ControllerError> DacDriver::get_status(SystemStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Populate embedded DAC status message
    status.has_dac = true;
    status.dac.enabled = cached_status_.enabled;
    status.dac.mode = cached_status_.mode;
    status.dac.waveform_type = cached_status_.waveform_type;
    status.dac.frequency = cached_status_.frequency;
    status.dac.amplitude = cached_status_.amplitude;
    status.dac.error = cached_status_.error;
    status.dac.waveform_error = cached_status_.waveform_error;
    status.dac.timestamp_ms = cached_status_.timestamp_ms;

    return {};
}

std::expected<void, ControllerError> DacDriver::get_measurements(SystemStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Populate embedded measurements message
    status.has_dac_measurements = true;
    status.dac_measurements.voltage_output = cached_measurements_.voltage_output;

    return {};
}

std::expected<void, ControllerError> DacDriver::get_state(SystemStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Populate embedded state message
    status.has_dac_state = true;
    status.dac_state.voltage = current_voltage_;
    status.dac_state.waveform_type = current_waveform_type_;
    status.dac_state.frequency = current_frequency_;
    status.dac_state.amplitude = current_amplitude_;

    return {};
}

std::expected<void, ControllerError> DacDriver::set_state(const DACState& state) noexcept {
    std::lock_guard lock(mutex_);

    // With required fields, all fields are always present
    current_voltage_ = state.voltage;
    current_waveform_type_ = state.waveform_type;
    current_frequency_ = state.frequency;
    current_amplitude_ = state.amplitude;

    trigger_update();
    return {};
}

std::expected<void, ControllerError> DacDriver::set_config(const DACConfigRequest& config) noexcept {
    std::lock_guard lock(mutex_);

    // With required fields in DACConfigRequest, all fields are always present
    current_mode_ = static_cast<DACMode>(config.mode);
    current_voltage_ = config.voltage;
    current_waveform_type_ = config.waveform_type;
    current_frequency_ = config.frequency;
    current_amplitude_ = config.amplitude;

    trigger_update();
    return {};
}

bool DacDriver::has_updates() const noexcept {
    return update_sem_.try_acquire();
}

bool DacDriver::wait_for_update(uint32_t timeout_ms) const noexcept {
    if (timeout_ms == 0) {
        update_sem_.acquire();
        return true;
    }
    return update_sem_.try_acquire_for(timeout_ms);
}

void DacDriver::trigger_update() noexcept {
    last_update_ms_ = osKernelGetTickCount();
    
    // Update cached status
    cached_status_.enabled = (is_enabled().value_or(false));
    cached_status_.mode = current_mode_;
    cached_status_.waveform_type = current_waveform_type_;
    cached_status_.frequency = current_frequency_;
    cached_status_.amplitude = current_amplitude_;
    cached_status_.timestamp_ms = last_update_ms_;
    
    update_sem_.release();
}

bool DacDriver::is_initialized() const noexcept {
    return (dac_handle_ != nullptr);
}

bool DacDriver::has_error() const noexcept {
    return false;  // TODO: Implement error detection
}

bool DacDriver::has_waveform_error() const noexcept {
    return false;  // TODO: Implement waveform error detection
}
