#ifndef __ERROR_HPP__
#define __ERROR_HPP__

#include "stm32g4xx_hal.h"
#include "bq25792.hpp"
#include "tps55289.hpp"
#include <expected>
#include <system_error>
#include <type_traits>
#include <string>

// =============================================================================
// Controller Error Enum
// =============================================================================

enum class ControllerError : uint8_t {
    // HAL errors
    HAL_OK = 0,
    HAL_ERROR,
    HAL_BUSY,
    HAL_TIMEOUT,

    // BQ25792 driver errors
    BQ25792_None = 10,
    BQ25792_HALError,
    BQ25792_Timeout,
    BQ25792_I2CError,
    BQ25792_InvalidAddress,
    BQ25792_InvalidParameter,
    BQ25792_NotInitialized,
    BQ25792_BatteryNotPresent,
    BQ25792_FaultDetected,

    // TPS55289 driver errors
    TPS55289_None = 20,
    TPS55289_HALError,
    TPS55289_Timeout,
    TPS55289_InvalidAddress,
    TPS55289_InvalidRegister,
    TPS55289_NotInitialized,
    TPS55289_I2CError,

    // DAC driver errors
    DAC_None = 30,
    DAC_ERROR,
    DAC_TIMEOUT,
    DAC_INVALID_MODE,
    DAC_INVALID_PARAMETER,
    DAC_NOT_INITIALIZED,
    DAC_DMA_ERROR,
    DAC_WAVEFORM_ERROR,

    // Controller-specific errors
    UnknownError = 100,
};

// =============================================================================
// Error Category Classes
// =============================================================================

class ControllerErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

class BQ25792ErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

class TPS55289ErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

class DACErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override;
    std::string message(int ev) const override;
};

// =============================================================================
// Error Category Instance Accessors
// =============================================================================

const std::error_category& controller_error_category();
const std::error_category& bq25792_error_category();
const std::error_category& tps55289_error_category();
const std::error_category& dac_error_category();

// =============================================================================
// Error Condition
// =============================================================================

std::error_condition make_error_condition(ControllerError);

namespace std {
    template <>
    struct is_error_condition_enum<ControllerError> : public std::true_type {};
}

// =============================================================================
// HAL Status Conversion
// =============================================================================

ControllerError from_hal_status(HAL_StatusTypeDef status);

template<typename T>
std::expected<T, ControllerError> hal_error(HAL_StatusTypeDef status) {
    return std::unexpected(from_hal_status(status));
}

// =============================================================================
// External Driver Error Conversions (Implicit)
// =============================================================================

// Convert from BQ25792 ErrorCode to ControllerError
ControllerError from_bq25792_error(bq25792::ErrorCode ec);

// Convert from std::error_code (with bq25792 category) to ControllerError
ControllerError from_bq25792_error(const std::error_code& ec);

// Convert from TPS55289 ErrorCode to ControllerError
ControllerError from_tps55289_error(tps55289::ErrorCode ec);

// Convert from std::error_code (with tps55289 category) to ControllerError
ControllerError from_tps55289_error(const std::error_code& ec);

// Convert from DAC ErrorCode to ControllerError
ControllerError from_dac_error(ControllerError ec);

// Convert from std::error_code (with dac category) to ControllerError
ControllerError from_dac_error(const std::error_code& ec);

// Implicit conversion via make_error_code
inline std::error_code make_error_code(ControllerError ec) {
    if (ec >= ControllerError::DAC_None && ec <= ControllerError::DAC_WAVEFORM_ERROR) {
        return std::error_code(static_cast<int>(ec), dac_error_category());
    }
    return std::error_code(static_cast<int>(ec), controller_error_category());
}

inline std::error_code make_error_code(bq25792::ErrorCode ec) {
    return std::error_code(static_cast<int>(from_bq25792_error(ec)), controller_error_category());
}

inline std::error_code make_error_code(tps55289::ErrorCode ec) {
    return std::error_code(static_cast<int>(from_tps55289_error(ec)), controller_error_category());
}

// =============================================================================
// Helper Functions
// =============================================================================

// Get source information as string
std::string get_error_source(const std::error_code& ec);

// Check if error came from specific driver
bool is_bq25792_error(const std::error_code& ec);
bool is_tps55289_error(const std::error_code& ec);
bool is_dac_error(const std::error_code& ec);
bool is_hal_error(const std::error_code& ec);

#endif
