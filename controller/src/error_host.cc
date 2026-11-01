#include "error_host.hpp"
#include "protocol.pb.h"
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
        case ControllerError::OscilloscopeAborted:
            return "Oscilloscope capture aborted";
        case ControllerError::OscilloscopeOutOfRange:
            return "Oscilloscope samples outside the captured window";

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

        case ControllerError::PSRAM_None:
            return "PSRAM no error";
        case ControllerError::PSRAM_HALError:
            return "PSRAM HAL error";
        case ControllerError::PSRAM_Timeout:
            return "PSRAM timeout";
        case ControllerError::PSRAM_InvalidAddress:
            return "PSRAM invalid address";
        case ControllerError::PSRAM_InvalidSize:
            return "PSRAM invalid size";
        case ControllerError::PSRAM_NotInitialized:
            return "PSRAM not initialized";
        case ControllerError::PSRAM_QSPIError:
            return "PSRAM QSPI transfer error";
        case ControllerError::PSRAM_ClockTooSlow:
            return "PSRAM QSPI clock too slow for tCEM";
        case ControllerError::PSRAM_ClockTooFast:
            return "PSRAM QSPI clock above limit";
        case ControllerError::PSRAM_ConfigError:
            return "PSRAM QUADSPI configuration invalid (FSIZE / CS high time)";
        case ControllerError::PSRAM_NotSupported:
            return "PSRAM operation not supported";
        case ControllerError::PSRAM_IdMismatch:
            return "PSRAM ID mismatch";
        case ControllerError::PSRAM_VerifyFailed:
            return "PSRAM read-back verification failed";
        case ControllerError::PSRAM_Busy:
            return "PSRAM busy (DMA streaming active)";
        case ControllerError::MCP44XX_None:
            return "MCP44xx no error";
        case ControllerError::MCP44XX_HALError:
            return "MCP44xx HAL error";
        case ControllerError::MCP44XX_Timeout:
            return "MCP44xx timeout";
        case ControllerError::MCP44XX_InvalidParameter:
            return "MCP44xx invalid parameter";
        case ControllerError::MCP44XX_InvalidWiper:
            return "MCP44xx invalid wiper";
        case ControllerError::MCP44XX_InvalidSlot:
            return "MCP44xx invalid EEPROM slot";
        case ControllerError::MCP44XX_EEPROMBusy:
            return "MCP44xx EEPROM write in progress";
        case ControllerError::Peripheral_None:
            return "Peripheral no error";
        case ControllerError::Peripheral_NotConfigured:
            return "Peripheral not configured (check CubeMX setup)";
        case ControllerError::Peripheral_InvalidParameter:
            return "Peripheral invalid parameter";
        case ControllerError::Peripheral_Busy:
            return "Peripheral busy";
        case ControllerError::Peripheral_NotStarted:
            return "Peripheral not started";
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
    if (ec >= ControllerError::OscilloscopeNotConfigured && ec <= ControllerError::OscilloscopeOutOfRange) {
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

// =============================================================================
// Wire format check: ControllerError must match ControllerErrorCode in protocol.proto
// =============================================================================

#define CHECK_WIRE_VALUE(name, proto) \
    static_assert(static_cast<int>(ControllerError::name) == static_cast<int>(ControllerErrorCode_##proto), \
                  "ControllerError::" #name " does not match protocol.proto")
CHECK_WIRE_VALUE(HAL_OK, CONTROLLER_ERROR_HAL_OK);
CHECK_WIRE_VALUE(HAL_ERROR, CONTROLLER_ERROR_HAL_ERROR);
CHECK_WIRE_VALUE(HAL_BUSY, CONTROLLER_ERROR_HAL_BUSY);
CHECK_WIRE_VALUE(HAL_TIMEOUT, CONTROLLER_ERROR_HAL_TIMEOUT);
CHECK_WIRE_VALUE(BQ25792_None, CONTROLLER_ERROR_BQ25792_NONE);
CHECK_WIRE_VALUE(BQ25792_HALError, CONTROLLER_ERROR_BQ25792_HAL_ERROR);
CHECK_WIRE_VALUE(BQ25792_Timeout, CONTROLLER_ERROR_BQ25792_TIMEOUT);
CHECK_WIRE_VALUE(BQ25792_I2CError, CONTROLLER_ERROR_BQ25792_I2C_ERROR);
CHECK_WIRE_VALUE(BQ25792_InvalidAddress, CONTROLLER_ERROR_BQ25792_INVALID_ADDRESS);
CHECK_WIRE_VALUE(BQ25792_InvalidParameter, CONTROLLER_ERROR_BQ25792_INVALID_PARAMETER);
CHECK_WIRE_VALUE(BQ25792_NotInitialized, CONTROLLER_ERROR_BQ25792_NOT_INITIALIZED);
CHECK_WIRE_VALUE(BQ25792_BatteryNotPresent, CONTROLLER_ERROR_BQ25792_BATTERY_NOT_PRESENT);
CHECK_WIRE_VALUE(BQ25792_FaultDetected, CONTROLLER_ERROR_BQ25792_FAULT_DETECTED);
CHECK_WIRE_VALUE(TPS55289_None, CONTROLLER_ERROR_TPS55289_NONE);
CHECK_WIRE_VALUE(TPS55289_HALError, CONTROLLER_ERROR_TPS55289_HAL_ERROR);
CHECK_WIRE_VALUE(TPS55289_Timeout, CONTROLLER_ERROR_TPS55289_TIMEOUT);
CHECK_WIRE_VALUE(TPS55289_InvalidAddress, CONTROLLER_ERROR_TPS55289_INVALID_ADDRESS);
CHECK_WIRE_VALUE(TPS55289_InvalidRegister, CONTROLLER_ERROR_TPS55289_INVALID_REGISTER);
CHECK_WIRE_VALUE(TPS55289_NotInitialized, CONTROLLER_ERROR_TPS55289_NOT_INITIALIZED);
CHECK_WIRE_VALUE(TPS55289_I2CError, CONTROLLER_ERROR_TPS55289_I2C_ERROR);
CHECK_WIRE_VALUE(TPS55289_InvalidParameter, CONTROLLER_ERROR_TPS55289_INVALID_PARAMETER);
CHECK_WIRE_VALUE(DAC_None, CONTROLLER_ERROR_DAC_NONE);
CHECK_WIRE_VALUE(DAC_ERROR, CONTROLLER_ERROR_DAC_ERROR);
CHECK_WIRE_VALUE(DAC_TIMEOUT, CONTROLLER_ERROR_DAC_TIMEOUT);
CHECK_WIRE_VALUE(DAC_INVALID_MODE, CONTROLLER_ERROR_DAC_INVALID_MODE);
CHECK_WIRE_VALUE(DAC_INVALID_PARAMETER, CONTROLLER_ERROR_DAC_INVALID_PARAMETER);
CHECK_WIRE_VALUE(DAC_NOT_INITIALIZED, CONTROLLER_ERROR_DAC_NOT_INITIALIZED);
CHECK_WIRE_VALUE(DAC_DMA_ERROR, CONTROLLER_ERROR_DAC_DMA_ERROR);
CHECK_WIRE_VALUE(DAC_WAVEFORM_ERROR, CONTROLLER_ERROR_DAC_WAVEFORM_ERROR);
CHECK_WIRE_VALUE(UnknownError, CONTROLLER_ERROR_UNKNOWN_ERROR);
CHECK_WIRE_VALUE(OscilloscopeNotConfigured, CONTROLLER_ERROR_OSCILLOSCOPE_NOT_CONFIGURED);
CHECK_WIRE_VALUE(OscilloscopeBufferOverflow, CONTROLLER_ERROR_OSCILLOSCOPE_BUFFER_OVERFLOW);
CHECK_WIRE_VALUE(OscilloscopeBufferEmpty, CONTROLLER_ERROR_OSCILLOSCOPE_BUFFER_EMPTY);
CHECK_WIRE_VALUE(OscilloscopeOverrun, CONTROLLER_ERROR_OSCILLOSCOPE_OVERRUN);
CHECK_WIRE_VALUE(OscilloscopeInvalidChannel, CONTROLLER_ERROR_OSCILLOSCOPE_INVALID_CHANNEL);
CHECK_WIRE_VALUE(OscilloscopeDMAError, CONTROLLER_ERROR_OSCILLOSCOPE_DMA_ERROR);
CHECK_WIRE_VALUE(OscilloscopeAborted, CONTROLLER_ERROR_OSCILLOSCOPE_ABORTED);
CHECK_WIRE_VALUE(OscilloscopeOutOfRange, CONTROLLER_ERROR_OSCILLOSCOPE_OUT_OF_RANGE);
CHECK_WIRE_VALUE(I2CArbitrationLost, CONTROLLER_ERROR_I2C_ARBITRATION_LOST);
CHECK_WIRE_VALUE(I2CNACK, CONTROLLER_ERROR_I2C_NACK);
CHECK_WIRE_VALUE(I2CBusError, CONTROLLER_ERROR_I2C_BUS_ERROR);
CHECK_WIRE_VALUE(MemoryAllocationFailed, CONTROLLER_ERROR_MEMORY_ALLOCATION_FAILED);
CHECK_WIRE_VALUE(ADCConversionError, CONTROLLER_ERROR_ADC_CONVERSION_ERROR);
CHECK_WIRE_VALUE(ADCBufferEmpty, CONTROLLER_ERROR_ADC_BUFFER_EMPTY);
CHECK_WIRE_VALUE(ADCBufferOverrun, CONTROLLER_ERROR_ADC_BUFFER_OVERRUN);
CHECK_WIRE_VALUE(PSRAM_None, CONTROLLER_ERROR_PSRAM_NONE);
CHECK_WIRE_VALUE(PSRAM_HALError, CONTROLLER_ERROR_PSRAM_HAL_ERROR);
CHECK_WIRE_VALUE(PSRAM_Timeout, CONTROLLER_ERROR_PSRAM_TIMEOUT);
CHECK_WIRE_VALUE(PSRAM_InvalidAddress, CONTROLLER_ERROR_PSRAM_INVALID_ADDRESS);
CHECK_WIRE_VALUE(PSRAM_InvalidSize, CONTROLLER_ERROR_PSRAM_INVALID_SIZE);
CHECK_WIRE_VALUE(PSRAM_NotInitialized, CONTROLLER_ERROR_PSRAM_NOT_INITIALIZED);
CHECK_WIRE_VALUE(PSRAM_QSPIError, CONTROLLER_ERROR_PSRAM_QSPI_ERROR);
CHECK_WIRE_VALUE(PSRAM_ClockTooSlow, CONTROLLER_ERROR_PSRAM_CLOCK_TOO_SLOW);
CHECK_WIRE_VALUE(PSRAM_ClockTooFast, CONTROLLER_ERROR_PSRAM_CLOCK_TOO_FAST);
CHECK_WIRE_VALUE(PSRAM_ConfigError, CONTROLLER_ERROR_PSRAM_CONFIG_ERROR);
CHECK_WIRE_VALUE(PSRAM_NotSupported, CONTROLLER_ERROR_PSRAM_NOT_SUPPORTED);
CHECK_WIRE_VALUE(PSRAM_IdMismatch, CONTROLLER_ERROR_PSRAM_ID_MISMATCH);
CHECK_WIRE_VALUE(PSRAM_VerifyFailed, CONTROLLER_ERROR_PSRAM_VERIFY_FAILED);
CHECK_WIRE_VALUE(PSRAM_Busy, CONTROLLER_ERROR_PSRAM_BUSY);
CHECK_WIRE_VALUE(MCP44XX_None, CONTROLLER_ERROR_MCP44XX_NONE);
CHECK_WIRE_VALUE(MCP44XX_HALError, CONTROLLER_ERROR_MCP44XX_HAL_ERROR);
CHECK_WIRE_VALUE(MCP44XX_Timeout, CONTROLLER_ERROR_MCP44XX_TIMEOUT);
CHECK_WIRE_VALUE(MCP44XX_InvalidParameter, CONTROLLER_ERROR_MCP44XX_INVALID_PARAMETER);
CHECK_WIRE_VALUE(MCP44XX_InvalidWiper, CONTROLLER_ERROR_MCP44XX_INVALID_WIPER);
CHECK_WIRE_VALUE(MCP44XX_InvalidSlot, CONTROLLER_ERROR_MCP44XX_INVALID_SLOT);
CHECK_WIRE_VALUE(MCP44XX_EEPROMBusy, CONTROLLER_ERROR_MCP44XX_EEPROM_BUSY);
CHECK_WIRE_VALUE(Peripheral_None, CONTROLLER_ERROR_PERIPHERAL_NONE);
CHECK_WIRE_VALUE(Peripheral_NotConfigured, CONTROLLER_ERROR_PERIPHERAL_NOT_CONFIGURED);
CHECK_WIRE_VALUE(Peripheral_InvalidParameter, CONTROLLER_ERROR_PERIPHERAL_INVALID_PARAMETER);
CHECK_WIRE_VALUE(Peripheral_Busy, CONTROLLER_ERROR_PERIPHERAL_BUSY);
CHECK_WIRE_VALUE(Peripheral_NotStarted, CONTROLLER_ERROR_PERIPHERAL_NOT_STARTED);
#undef CHECK_WIRE_VALUE
