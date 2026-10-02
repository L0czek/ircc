#include "dac_controller.hpp"
#include "log.hpp"
#include "protocol.pb.h"
#include "cmsis_os2.h"
#include "board.h"
#include "dac_waveform.hpp"
#include <cstring>
#include <mutex>

DACController::DACController(DAC_HandleTypeDef* dac_handles[4])
    : driver_(dac_handles),
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
}

DACController::~DACController() = default;

std::expected<void, ControllerError> DACController::initialize() noexcept {
    auto result = driver_.initialize();
    if (!result) {
        os::error("Failed to initialize DAC: %d\n", static_cast<int>(result.error()));
        return std::unexpected(result.error());
    }
    return {};
}

std::expected<void, ControllerError> DACController::set_enabled(bool enable) noexcept {
    std::lock_guard lock(mutex_);

    for (uint8_t dac_num = 0; dac_num < 4; ++dac_num) {
        if (enable) {
            auto result = driver_.enable(dac_num);
            if (!result) {
                os::error("Failed to enable DAC%d: %d\n", dac_num + 1, static_cast<int>(result.error()));
                return std::unexpected(result.error());
            }
        } else {
            auto result = driver_.disable(dac_num);
            if (!result) {
                os::error("Failed to disable DAC%d: %d\n", dac_num + 1, static_cast<int>(result.error()));
                return std::unexpected(result.error());
            }
        }
    }

    trigger_update();
    return {};
}

std::expected<bool, ControllerError> DACController::is_enabled() const noexcept {
    std::lock_guard lock(mutex_);

    // Check if any DAC is enabled (assuming all are in same state)
    for (uint8_t dac_num = 0; dac_num < 4; ++dac_num) {
        auto result = driver_.is_enabled(dac_num);
        if (result) {
            return result.value();  // Return first DAC state
        }
    }

    return false;  // Default if all fail
}

std::expected<void, ControllerError> DACController::set_mode(DACMode mode) noexcept {
    std::lock_guard lock(mutex_);

    // Stop waveform if switching from waveform mode
    if (current_mode_ == DACMode_DAC_MODE_WAVEFORM && mode == DACMode_DAC_MODE_VOLTAGE) {
        auto result = driver_.stop_waveform(0);  // Stop DMA on DAC1
        if (!result) {
            os::error("Failed to stop waveform: %d\n", static_cast<int>(result.error()));
            return std::unexpected(result.error());
        }
    }

    current_mode_ = mode;
    trigger_update();
    return {};
}

DACMode DACController::get_mode() const noexcept {
    std::lock_guard lock(mutex_);
    return current_mode_;
}

std::expected<void, ControllerError> DACController::set_voltage(float voltage) noexcept {
    std::lock_guard lock(mutex_);

    current_voltage_ = voltage;

    // Set voltage on all DAC channels
    for (uint8_t dac_num = 0; dac_num < 4; ++dac_num) {
        auto result = driver_.set_voltage(dac_num, voltage);
        if (!result) {
            os::error("Failed to set DAC%d voltage: %d\n", dac_num + 1, static_cast<int>(result.error()));
            return std::unexpected(result.error());
        }
    }

    trigger_update();
    return {};
}

std::expected<float, ControllerError> DACController::get_voltage() const noexcept {
    std::lock_guard lock(mutex_);
    return current_voltage_;
}

std::expected<void, ControllerError> DACController::set_waveform(
    WaveformType waveform_type,
    float frequency,
    float amplitude
) noexcept {
    std::lock_guard lock(mutex_);

    current_waveform_type_ = waveform_type;
    current_frequency_ = frequency;
    current_amplitude_ = amplitude;

    // Configure waveform on all DAC channels
    for (uint8_t dac_num = 0; dac_num < 4; ++dac_num) {
        auto result = driver_.set_waveform(dac_num, static_cast<uint8_t>(waveform_type), frequency, amplitude);
        if (!result) {
            os::error("Failed to configure DAC%d waveform: %d\n", dac_num + 1, static_cast<int>(result.error()));
            return std::unexpected(result.error());
        }
    }

    trigger_update();
    return {};
}

std::expected<void, ControllerError> DACController::get_waveform(
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

std::expected<void, ControllerError> DACController::start_waveform() noexcept {
    std::lock_guard lock(mutex_);

    // Start DMA waveform generation on all DAC channels
    for (uint8_t dac_num = 0; dac_num < 4; ++dac_num) {
        auto result = driver_.start_waveform(dac_num);
        if (!result) {
            os::error("Failed to start DAC%d waveform: %d\n", dac_num + 1, static_cast<int>(result.error()));
            return std::unexpected(result.error());
        }
    }

    trigger_update();
    return {};
}

std::expected<void, ControllerError> DACController::stop_waveform() noexcept {
    std::lock_guard lock(mutex_);

    // Stop DMA waveform generation on all DAC channels
    for (uint8_t dac_num = 0; dac_num < 4; ++dac_num) {
        auto result = driver_.stop_waveform(dac_num);
        if (!result) {
            os::error("Failed to stop DAC%d waveform: %d\n", dac_num + 1, static_cast<int>(result.error()));
            return std::unexpected(result.error());
        }
    }

    trigger_update();
    return {};
}

std::expected<void, ControllerError> DACController::get_status(DACStatus& status) const noexcept {
    std::lock_guard lock(mutex_);

    // Fill in status
    status.enabled = is_enabled().value_or(false);
    status.mode = current_mode_;
    status.waveform_type = current_waveform_type_;
    status.frequency = current_frequency_;
    status.amplitude = current_amplitude_;
    status.error = false;  // TODO: Implement error detection
    status.waveform_error = false;  // TODO: Implement waveform error detection
    status.timestamp_ms = last_update_ms_;

    return {};
}

std::expected<void, ControllerError> DACController::get_measurements(DACMeasurements& measurements) const noexcept {
    std::lock_guard lock(mutex_);

    // TODO: Implement actual voltage measurement
    // For now, use cached voltage value
    measurements.voltage_output = current_voltage_;

    return {};
}

std::expected<void, ControllerError> DACController::get_config(DACConfig& config) const noexcept {
    std::lock_guard lock(mutex_);

    config.mode = current_mode_;
    config.voltage = current_voltage_;
    config.waveform_type = current_waveform_type_;
    config.frequency = current_frequency_;
    config.amplitude = current_amplitude_;

    return {};
}

std::expected<void, ControllerError> DACController::set_config(const DACConfig& config) noexcept {
    std::lock_guard lock(mutex_);

    if (config.mode) {
        current_mode_ = static_cast<DACMode>(config.mode);
    }

    if (config.voltage) {
        current_voltage_ = config.voltage;
    }

    if (config.waveform_type) {
        current_waveform_type_ = config.waveform_type;
    }

    if (config.frequency) {
        current_frequency_ = config.frequency;
    }

    if (config.amplitude) {
        current_amplitude_ = config.amplitude;
    }

    trigger_update();
    return {};
}

std::expected<void, ControllerError> DACController::set_config(const DACConfigRequest& config) noexcept {
    std::lock_guard lock(mutex_);

    if (config.mode) {
        current_mode_ = static_cast<DACMode>(config.mode);
    }

    if (config.voltage) {
        current_voltage_ = config.voltage;
    }

    if (config.waveform_type) {
        current_waveform_type_ = config.waveform_type;
    }

    if (config.frequency) {
        current_frequency_ = config.frequency;
    }

    if (config.amplitude) {
        current_amplitude_ = config.amplitude;
    }

    trigger_update();
    return {};
}

bool DACController::has_updates() const noexcept {
    return update_sem_.try_acquire();
}

bool DACController::wait_for_update(uint32_t timeout_ms) const noexcept {
    if (timeout_ms == 0) {
        update_sem_.acquire();
        return true;
    }
    return update_sem_.try_acquire_for(timeout_ms);
}

void DACController::trigger_update() noexcept {
    last_update_ms_ = osKernelGetTickCount();
    update_sem_.release();
}

bool DACController::is_initialized() const noexcept {
    // If we got here, we're initialized
    return true;
}

bool DACController::has_error() const noexcept {
    std::lock_guard lock(mutex_);
    return false;  // TODO: Implement error detection
}

bool DACController::has_waveform_error() const noexcept {
    std::lock_guard lock(mutex_);
    return false;  // TODO: Implement waveform error detection
}
