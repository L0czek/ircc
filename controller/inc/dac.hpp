#ifndef __DAC_DRV_HPP__
#define __DAC_DRV_HPP__

#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_dac.h"
#include "error.hpp"
#include <cstdint>
#include <expected>

/**
 * @brief DAC driver wrapper
 *
 * Wraps STM32 HAL DAC handles for DAC1-DAC4.
 * Supports constant voltage mode and waveform generation mode.
 * Waveform generation can use DMA circular mode when configured.
 */
class DacDriver {
    DAC_HandleTypeDef* dac_handles_[4];

public:
    /**
     * @brief Construct DAC driver
     * @param handles Array of 4 DAC_HandleTypeDef pointers (DAC2 may be nullptr for smaller packages)
     */
    explicit DacDriver(DAC_HandleTypeDef* handles[4]);

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
     * @brief Enable a specific DAC channel
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> enable(uint8_t dac_num) noexcept;

    /**
     * @brief Disable a specific DAC channel
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> disable(uint8_t dac_num) noexcept;

    /**
     * @brief Set output voltage for a specific DAC channel
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @param voltage Output voltage in volts (0-3.3V)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_voltage(uint8_t dac_num, float voltage) noexcept;

    /**
     * @brief Set waveform type for a specific DAC channel
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @param waveform_type Waveform type (sine, square, triangle, etc.)
     * @param frequency Waveform frequency in Hz
     * @param amplitude Waveform amplitude in volts
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> set_waveform(
        uint8_t dac_num,
        uint8_t waveform_type,
        float frequency,
        float amplitude
    ) noexcept;

    /**
     * @brief Start waveform generation using DMA circular mode
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> start_waveform(uint8_t dac_num) noexcept;

    /**
     * @brief Stop waveform generation
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @return std::expected<void, ControllerError> - success on void, error on failure
     */
    std::expected<void, ControllerError> stop_waveform(uint8_t dac_num) noexcept;

    /**
     * @brief Check if a specific DAC channel is enabled
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @return std::expected<bool, ControllerError> - true if enabled, error on failure
     */
    std::expected<bool, ControllerError> is_enabled(uint8_t dac_num) const noexcept;

    /**
     * @brief Get the DAC handle for a specific channel
     * @param dac_num DAC channel number (0-3 for DAC1-DAC4)
     * @return DAC_HandleTypeDef* - handle pointer (may be nullptr for unavailable channels)
     */
    DAC_HandleTypeDef* get_handle(uint8_t dac_num) const noexcept;
};

#endif
