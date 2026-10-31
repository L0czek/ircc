#include "error_host.hpp"
#include <cstdint>
#include <string>

// =============================================================================
// Error Message Implementation (Host version)
// =============================================================================

std::string get_error_message(ControllerError error) {
    switch (error) {
        case ControllerError::HAL_OK:
            return "HAL operation successful";
        case ControllerError::HAL_ERROR:
            return "HAL operation error";
        case ControllerError::HAL_BUSY:
            return "HAL busy";
        case ControllerError::HAL_TIMEOUT:
            return "HAL timeout";

        case ControllerError::BQ25792_None:
            return "BQ25792 no error";
        case ControllerError::BQ25792_HALError:
            return "BQ25792 HAL error";
        case ControllerError::BQ25792_Timeout:
            return "BQ25792 timeout";
        case ControllerError::BQ25792_I2CError:
            return "BQ25792 I2C communication error";
        case ControllerError::BQ25792_InvalidAddress:
            return "BQ25792 invalid register address";
        case ControllerError::BQ25792_InvalidParameter:
            return "BQ25792 invalid parameter";
        case ControllerError::BQ25792_NotInitialized:
            return "BQ25792 not initialized";
        case ControllerError::BQ25792_BatteryNotPresent:
            return "BQ25792 battery not present";
        case ControllerError::BQ25792_FaultDetected:
            return "BQ25792 fault condition detected";

        case ControllerError::TPS55289_None:
            return "TPS55289 no error";
        case ControllerError::TPS55289_HALError:
            return "TPS55289 HAL error";
        case ControllerError::TPS55289_Timeout:
            return "TPS55289 timeout";
        case ControllerError::TPS55289_InvalidAddress:
            return "TPS55289 invalid I2C address";
        case ControllerError::TPS55289_InvalidRegister:
            return "TPS55289 invalid register address";
        case ControllerError::TPS55289_NotInitialized:
            return "TPS55289 not initialized";
        case ControllerError::TPS55289_I2CError:
            return "TPS55289 I2C communication error";
        case ControllerError::TPS55289_InvalidParameter:
            return "TPS55289 invalid parameter";

        case ControllerError::DAC_None:
            return "DAC no error";
        case ControllerError::DAC_ERROR:
            return "DAC general error";
        case ControllerError::DAC_TIMEOUT:
            return "DAC timeout";
        case ControllerError::DAC_INVALID_MODE:
            return "DAC invalid mode";
        case ControllerError::DAC_INVALID_PARAMETER:
            return "DAC invalid parameter";
        case ControllerError::DAC_NOT_INITIALIZED:
            return "DAC not initialized";
        case ControllerError::DAC_DMA_ERROR:
            return "DAC DMA error";
        case ControllerError::DAC_WAVEFORM_ERROR:
            return "DAC waveform generation error";

        case ControllerError::OscilloscopeNotConfigured:
            return "Oscilloscope not configured";
        case ControllerError::OscilloscopeBufferOverflow:
            return "Oscilloscope buffer overflow";
        case ControllerError::OscilloscopeBufferEmpty:
            return "Oscilloscope buffer empty";
        case ControllerError::OscilloscopeOverrun:
            return "Oscilloscope overrun";
        case ControllerError::OscilloscopeInvalidChannel:
            return "Oscilloscope invalid channel";
        case ControllerError::OscilloscopeDMAError:
            return "Oscilloscope DMA error";

        case ControllerError::I2CArbitrationLost:
            return "I2C arbitration lost";
        case ControllerError::I2CNACK:
            return "I2C NACK received";
        case ControllerError::I2CBusError:
            return "I2C bus error";

        case ControllerError::MemoryAllocationFailed:
            return "Memory allocation failed";

        case ControllerError::ADCConversionError:
            return "ADC conversion error";
        case ControllerError::ADCBufferEmpty:
            return "ADC buffer empty";
        case ControllerError::ADCBufferOverrun:
            return "ADC buffer overrun";

        case ControllerError::UnknownError:
        default:
            return "Unknown error";
    }
}

// =============================================================================
// External Driver Error Conversions (Host version - simplified)
// =============================================================================

ControllerError from_bq25792_error(uint8_t ec) {
    switch (static_cast<ControllerError>(ec)) {
        case ControllerError::BQ25792_None:
        case ControllerError::BQ25792_HALError:
        case ControllerError::BQ25792_Timeout:
        case ControllerError::BQ25792_I2CError:
        case ControllerError::BQ25792_InvalidAddress:
        case ControllerError::BQ25792_InvalidParameter:
        case ControllerError::BQ25792_NotInitialized:
        case ControllerError::BQ25792_BatteryNotPresent:
        case ControllerError::BQ25792_FaultDetected:
            return static_cast<ControllerError>(ec);
        default:
            return ControllerError::I2CBusError;
    }
}

ControllerError from_tps55289_error(uint8_t ec) {
    switch (static_cast<ControllerError>(ec)) {
        case ControllerError::TPS55289_None:
        case ControllerError::TPS55289_HALError:
        case ControllerError::TPS55289_Timeout:
        case ControllerError::TPS55289_InvalidAddress:
        case ControllerError::TPS55289_InvalidRegister:
        case ControllerError::TPS55289_NotInitialized:
        case ControllerError::TPS55289_I2CError:
        case ControllerError::TPS55289_InvalidParameter:
            return static_cast<ControllerError>(ec);
        default:
            return ControllerError::I2CBusError;
    }
}

ControllerError from_dac_error(ControllerError ec) {
    // DAC errors are already in ControllerError enum
    if (ec >= ControllerError::DAC_None && ec <= ControllerError::DAC_WAVEFORM_ERROR) {
        return ec;
    }
    // Check for other controller-specific errors
    if (ec >= ControllerError::OscilloscopeNotConfigured && ec <= ControllerError::OscilloscopeDMAError) {
        return ec;
    }
    if (ec >= ControllerError::I2CArbitrationLost && ec <= ControllerError::I2CBusError) {
        return ec;
    }
    if (ec >= ControllerError::MemoryAllocationFailed && ec <= ControllerError::ADCBufferOverrun) {
        return ec;
    }
    return ControllerError::UnknownError;
}
