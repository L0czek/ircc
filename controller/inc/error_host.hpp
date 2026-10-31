#ifndef __ERROR_HOST_HPP__
#define __ERROR_HOST_HPP__

#include <cstdint>
#include <expected>
#include <system_error>
#include <string>

// =============================================================================
// Controller Error Enum (Host version - no HAL dependencies)
// =============================================================================

enum class ControllerError : uint8_t {
    // HAL errors (mapped to reasonable defaults for host)
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
};

// =============================================================================
// Helper functions for host build (no HAL dependencies)
// =============================================================================

// Convert from BQ25792 ErrorCode to ControllerError
ControllerError from_bq25792_error(uint8_t ec);

// Convert from TPS55289 ErrorCode to ControllerError
ControllerError from_tps55289_error(uint8_t ec);

// Convert from DAC ErrorCode to ControllerError
ControllerError from_dac_error(ControllerError ec);

// Get error message as string
std::string get_error_message(ControllerError error);

#endif
