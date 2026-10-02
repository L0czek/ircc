#include "dac.hpp"
#include "log.hpp"
#include "board.h"
#include "dac_waveform.hpp"
#include <cstring>

DacDriver::DacDriver(DAC_HandleTypeDef* handles[4]) {
    // Copy handles (DAC2 may be nullptr for smaller packages)
    for (int i = 0; i < 4; ++i) {
        dac_handles_[i] = handles[i];
    }
}

DacDriver::~DacDriver() = default;

std::expected<void, ControllerError> DacDriver::initialize() noexcept {
    // DAC handles are initialized by CubeMX, so we just verify they're not null
    // DAC1, DAC3, DAC4 are always present on STM32G474
    // DAC2 is only present on 144-pin packages

    if (dac_handles_[0] == nullptr) {
        os::error("DAC1 handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    if (dac_handles_[2] == nullptr) {
        os::error("DAC3 handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    if (dac_handles_[3] == nullptr) {
        os::error("DAC4 handle is null\n");
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // DAC2 may be nullptr for smaller packages
    if (dac_handles_[1] == nullptr) {
        os::info("DAC2 not available (package may not have DAC2)\n");
    }

    return {};
}

std::expected<void, ControllerError> DacDriver::enable(uint8_t dac_num) noexcept {
    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    HAL_StatusTypeDef result = HAL_DAC_Start(dac_handles_[dac_num], dac_num);

    if (result != HAL_OK) {
        os::error("Failed to enable DAC%d: HAL error %d\n", dac_num + 1, result);
        return std::unexpected(from_hal_status(result));
    }

    return {};
}

std::expected<void, ControllerError> DacDriver::disable(uint8_t dac_num) noexcept {
    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    HAL_StatusTypeDef result = HAL_DAC_Stop(dac_handles_[dac_num], dac_num);

    if (result != HAL_OK) {
        os::error("Failed to disable DAC%d: HAL error %d\n", dac_num + 1, result);
        return std::unexpected(from_hal_status(result));
    }

    return {};
}

std::expected<void, ControllerError> DacDriver::set_voltage(uint8_t dac_num, float voltage) noexcept {
    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Clamp voltage to valid range (0-3.3V)
    if (voltage < 0.0f) voltage = 0.0f;
    if (voltage > 3.3f) voltage = 3.3f;

    // Convert voltage to DAC code (12-bit)
    uint32_t dac_code = static_cast<uint32_t>((voltage / 3.3f) * 4095 + 0.5f);

    HAL_StatusTypeDef result = HAL_DAC_SetValue(
        dac_handles_[dac_num],
        dac_num,
        DAC_ALIGN_12B_R,
        dac_code
    );

    if (result != HAL_OK) {
        os::error("Failed to set DAC%d voltage: HAL error %d\n", dac_num + 1, result);
        return std::unexpected(from_hal_status(result));
    }

    return {};
}

std::expected<void, ControllerError> DacDriver::set_waveform(
    uint8_t dac_num,
    uint8_t waveform_type,
    float frequency,
    float amplitude
) noexcept {
    [[maybe_unused]] uint8_t wt = waveform_type;  // Store for future use
    // This function configures software-based waveform generation
    // For DMA-based waveform generation, use start_waveform() after configuring DMA

    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
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

    // Store waveform configuration for later use
    // The actual waveform generation will be handled by start_waveform()

    return {};
}

std::expected<void, ControllerError> DacDriver::start_waveform(uint8_t dac_num) noexcept {
    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // For DMA circular mode, we need to:
    // 1. Ensure DAC is in waveform mode (configured via CubeMX)
    // 2. Start DMA channel for DAC
    // 3. Start DAC with DMA trigger

    // This is a placeholder - actual implementation depends on CubeMX configuration
    // The user is responsible for configuring DMA channels in CubeMX

    HAL_StatusTypeDef result = HAL_DAC_Start_DMA(
        dac_handles_[dac_num],
        dac_num,
        nullptr,  // DMA buffer - user must configure via CubeMX
        0,        // Buffer size - user must configure via CubeMX
        DAC_ALIGN_12B_R
    );

    if (result != HAL_OK) {
        os::error("Failed to start DAC%d DMA: HAL error %d\n", dac_num + 1, result);
        return std::unexpected(from_hal_status(result));
    }

    return {};
}

std::expected<void, ControllerError> DacDriver::stop_waveform(uint8_t dac_num) noexcept {
    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    HAL_StatusTypeDef result = HAL_DAC_Stop_DMA(dac_handles_[dac_num], dac_num);

    if (result != HAL_OK) {
        os::error("Failed to stop DAC%d DMA: HAL error %d\n", dac_num + 1, result);
        return std::unexpected(from_hal_status(result));
    }

    return {};
}

std::expected<bool, ControllerError> DacDriver::is_enabled(uint8_t dac_num) const noexcept {
    if (dac_num > 3) {
        os::error("Invalid DAC channel: %d\n", dac_num);
        return std::unexpected(ControllerError::DAC_INVALID_PARAMETER);
    }

    if (dac_handles_[dac_num] == nullptr) {
        os::error("DAC%d handle is null\n", dac_num + 1);
        return std::unexpected(ControllerError::DAC_ERROR);
    }

    // Check DAC state (this requires reading status register)
    // For now, we'll assume the HAL provides a way to check status
    // In practice, you may need to track enabled state yourself

    return true;  // Placeholder
}

DAC_HandleTypeDef* DacDriver::get_handle(uint8_t dac_num) const noexcept {
    if (dac_num > 3) {
        return nullptr;
    }
    return dac_handles_[dac_num];
}
