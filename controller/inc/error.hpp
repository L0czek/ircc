#ifndef __ERROR_HPP__
#define __ERROR_HPP__

#include "stm32g4xx_hal.h"
#include "bq25792.hpp"
#include "tps55289.hpp"
#include "aps6404l.hpp"
#include "mcp44xx.hpp"
#include <expected>
#include <system_error>
#include <type_traits>
#include <string>

// =============================================================================
// Controller Error Enum
// =============================================================================

// Values are mirrored by enum ControllerErrorCode in protocol.proto (sent to the
// host in Response.result); static_asserts in error.cc / error_host.cc keep the
// two in sync. Never renumber existing values.
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
    TPS55289_InvalidParameter,

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
    
    // Oscilloscope-specific errors
    OscilloscopeNotConfigured = 110,
    OscilloscopeBufferOverflow,
    OscilloscopeBufferEmpty,
    OscilloscopeOverrun,
    OscilloscopeInvalidChannel,
    OscilloscopeDMAError,
    OscilloscopeAborted,      // capture stopped before completion
    OscilloscopeOutOfRange,   // requested samples outside the captured window
    
    // I2C-specific errors
    I2CArbitrationLost = 120,
    I2CNACK,
    I2CBusError,
    
    // Memory/allocate errors
    MemoryAllocationFailed = 130,
    
    // ADC-specific errors
    ADCConversionError = 140,
    ADCBufferEmpty,
    ADCBufferOverrun,

    // APS6404L PSRAM driver errors
    PSRAM_None = 150,
    PSRAM_HALError,
    PSRAM_Timeout,
    PSRAM_InvalidAddress,
    PSRAM_InvalidSize,
    PSRAM_NotInitialized,
    PSRAM_QSPIError,
    PSRAM_ClockTooSlow,
    PSRAM_ClockTooFast,
    PSRAM_ConfigError,
    PSRAM_NotSupported,
    PSRAM_IdMismatch,
    PSRAM_VerifyFailed,
    PSRAM_Busy,

    // MCP44xx digital potentiometer driver errors (I2C failures map to I2C*)
    MCP44XX_None = 170,
    MCP44XX_HALError,
    MCP44XX_Timeout,
    MCP44XX_InvalidParameter,
    MCP44XX_InvalidWiper,
    MCP44XX_InvalidSlot,
    MCP44XX_EEPROMBusy,
};

// =============================================================================
// Error Category Classes
// =============================================================================

class ControllerErrorCategory : public std::error_category {
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
// Driver errors carry the driver's own category object (bq25792::get_error_category(),
// tps55289::get_error_category()); these accessors return exactly that object so
// category comparisons in from_*_error() match.
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

// I2C-specific error conversion
ControllerError from_i2c_error(HAL_StatusTypeDef status, uint32_t hal_i2c_error);

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
ControllerError from_psram_error(const std::error_code& ec);
ControllerError from_mcp44xx_error(const std::error_code& ec);

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


// =============================================================================
// Helper Functions
// =============================================================================

// Get source information as string
/**
 * @brief Static, allocation-free description of a ControllerError
 */
const char* to_string(ControllerError ec) noexcept;

/**
 * @brief Name of the component an error_code came from ("bq25792", "tps55289",
 *        "psram", "mcp44xx", "dac", "controller"). Static string, no allocation.
 */
const char* get_error_source(const std::error_code& ec) noexcept;

/**
 * @brief True for errors that indicate a disturbed or stuck I2C bus (as opposed
 *        to an absent device or a rejected parameter); bus recovery may help.
 */
bool is_i2c_bus_fault(ControllerError ec) noexcept;

/**
 * @brief Suppresses repeats of the same error from the same call site.
 *
 * Not thread safe: use one instance per lock domain. allow() returns true when
 * the error should be logged; suppressed() then reports how many identical
 * errors were dropped since the last logged one.
 */
class ErrorLogLimiter {
public:
    explicit ErrorLogLimiter(uint32_t window_ms = 1000) : window_ms_(window_ms) {}

    bool allow(const char* site, ControllerError ec, uint32_t now_ms) noexcept {
        if (site == last_site_ && ec == last_error_ && (now_ms - last_ms_) < window_ms_) {
            ++suppressed_;
            return false;
        }
        reported_suppressed_ = suppressed_;
        suppressed_ = 0;
        last_site_ = site;
        last_error_ = ec;
        last_ms_ = now_ms;
        return true;
    }

    uint32_t suppressed() const noexcept { return reported_suppressed_; }

private:
    uint32_t window_ms_;
    const char* last_site_ = nullptr;
    ControllerError last_error_ = ControllerError::HAL_OK;
    uint32_t last_ms_ = 0;
    uint32_t suppressed_ = 0;
    uint32_t reported_suppressed_ = 0;
};

// Check if error came from specific driver
bool is_bq25792_error(const std::error_code& ec);
bool is_tps55289_error(const std::error_code& ec);
bool is_dac_error(const std::error_code& ec);
bool is_hal_error(const std::error_code& ec);

#endif
