#include "error.hpp"
#include "stm32g4xx_hal.h"
#include <system_error>
#include <cstring>
#include "protocol.pb.h"

// =============================================================================
// ControllerErrorCategory Implementation
// =============================================================================

const char* ControllerErrorCategory::name() const noexcept {
    return "controller";
}

const char* to_string(ControllerError ec) noexcept {
    switch (ec) {
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

std::string ControllerErrorCategory::message(int error) const {
    return to_string(static_cast<ControllerError>(error));
}

const std::error_category& controller_error_category() {
    static ControllerErrorCategory category;
    return category;
}

std::error_condition make_error_condition(ControllerError e) {
    return std::error_condition(
        static_cast<int>(e),
        controller_error_category()
    );
}

// =============================================================================
// Driver error categories
//
// The drivers create their std::error_code values with their own category
// singletons. A separate controller-side copy of those categories never compares
// equal, which made every driver error decode as UnknownError / I2CBusError.
// =============================================================================

const std::error_category& bq25792_error_category() {
    return bq25792::get_error_category();
}

const std::error_category& tps55289_error_category() {
    return tps55289::get_error_category();
}

// =============================================================================
// DACErrorCategory Implementation
// =============================================================================

const char* DACErrorCategory::name() const noexcept {
    return "dac";
}

std::string DACErrorCategory::message(int error) const {
    switch (static_cast<ControllerError>(error)) {
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
        default:
            return "DAC unknown error";
    }
}

const std::error_category& dac_error_category() {
    static DACErrorCategory category;
    return category;
}

// =============================================================================
// HAL Status Conversion
// =============================================================================

ControllerError from_hal_status(HAL_StatusTypeDef status) {
    switch (status) {
        case HAL_OK:      return ControllerError::HAL_OK;
        case HAL_ERROR:   return ControllerError::HAL_ERROR;
        case HAL_BUSY:    return ControllerError::HAL_BUSY;
        case HAL_TIMEOUT: return ControllerError::HAL_TIMEOUT;
        default:          return ControllerError::UnknownError;
    }
}

// =============================================================================
// I2C Error Conversion (HAL-specific)
// =============================================================================

ControllerError from_i2c_error(HAL_StatusTypeDef status, uint32_t hal_i2c_error) {
    // hal_i2c_error is HAL_I2C_GetError(). A NACK is reported by HAL as HAL_ERROR
    // with the AF flag, not as a timeout. On this single-master bus ARLO cannot be
    // a real arbitration loss; like BERR it means the bus is disturbed or stuck.
    if (status == HAL_OK) {
        return ControllerError::HAL_OK;
    }
    if (hal_i2c_error & HAL_I2C_ERROR_AF) {
        return ControllerError::I2CNACK;
    }
    if (hal_i2c_error & (HAL_I2C_ERROR_BERR | HAL_I2C_ERROR_ARLO)) {
        return ControllerError::I2CBusError;
    }
    switch (status) {
        case HAL_TIMEOUT: return ControllerError::HAL_TIMEOUT;
        case HAL_BUSY:    return ControllerError::I2CBusError;
        default:          return ControllerError::HAL_ERROR;
    }
}

// =============================================================================
// External Driver Error Conversion
// =============================================================================

ControllerError from_bq25792_error(bq25792::ErrorCode ec) {
    switch (ec) {
        case bq25792::ErrorCode::None:           return ControllerError::BQ25792_None;
        case bq25792::ErrorCode::HALError:       return ControllerError::BQ25792_HALError;
        case bq25792::ErrorCode::Timeout:        return ControllerError::BQ25792_Timeout;
        case bq25792::ErrorCode::I2CError:       return ControllerError::BQ25792_I2CError;
        case bq25792::ErrorCode::InvalidAddress: return ControllerError::BQ25792_InvalidAddress;
        case bq25792::ErrorCode::InvalidParameter: return ControllerError::BQ25792_InvalidParameter;
        case bq25792::ErrorCode::NotInitialized: return ControllerError::BQ25792_NotInitialized;
        case bq25792::ErrorCode::BatteryNotPresent: return ControllerError::BQ25792_BatteryNotPresent;
        case bq25792::ErrorCode::FaultDetected:  return ControllerError::BQ25792_FaultDetected;
        case bq25792::ErrorCode::Nack:           return ControllerError::I2CNACK;
        case bq25792::ErrorCode::BusError:       return ControllerError::I2CBusError;
        default:                                 return ControllerError::UnknownError;
    }
}

ControllerError from_bq25792_error(const std::error_code& ec) {
    // The bq25792 driver's error_code stores the ErrorCode enum value directly
    if (ec.category() == bq25792_error_category()) {
        return from_bq25792_error(static_cast<bq25792::ErrorCode>(ec.value()));
    }
    // If already in controller category, just cast
    if (ec.category() == controller_error_category()) {
        auto val = static_cast<ControllerError>(ec.value());
        if (val >= ControllerError::BQ25792_None && val <= ControllerError::BQ25792_FaultDetected) {
            return val;
        }
        // Check for other controller-specific errors
        if (val >= ControllerError::OscilloscopeNotConfigured && val <= ControllerError::OscilloscopeOutOfRange) {
            return val;
        }
        if (val >= ControllerError::I2CArbitrationLost && val <= ControllerError::I2CBusError) {
            return val;
        }
        if (val >= ControllerError::MemoryAllocationFailed && val <= ControllerError::ADCBufferOverrun) {
            return val;
        }
    }
    return ControllerError::UnknownError;
}

ControllerError from_tps55289_error(tps55289::ErrorCode ec) {
    switch (ec) {
        case tps55289::ErrorCode::None:          return ControllerError::TPS55289_None;
        case tps55289::ErrorCode::HALError:      return ControllerError::TPS55289_HALError;
        case tps55289::ErrorCode::Timeout:       return ControllerError::TPS55289_Timeout;
        case tps55289::ErrorCode::InvalidAddress:return ControllerError::TPS55289_InvalidAddress;
        case tps55289::ErrorCode::InvalidRegister:return ControllerError::TPS55289_InvalidRegister;
        case tps55289::ErrorCode::NotInitialized:return ControllerError::TPS55289_NotInitialized;
        case tps55289::ErrorCode::I2CError:      return ControllerError::TPS55289_I2CError;
        case tps55289::ErrorCode::InvalidParameter: return ControllerError::TPS55289_InvalidParameter;
        case tps55289::ErrorCode::Nack:          return ControllerError::I2CNACK;
        case tps55289::ErrorCode::BusError:      return ControllerError::I2CBusError;
        default:                                 return ControllerError::UnknownError;
    }
}

ControllerError from_tps55289_error(const std::error_code& ec) {
    // The tps55289 driver's error_code stores the ErrorCode enum value directly
    if (ec.category() == tps55289_error_category()) {
        return from_tps55289_error(static_cast<tps55289::ErrorCode>(ec.value()));
    }
    // If already in controller category, just cast
    if (ec.category() == controller_error_category()) {
        auto val = static_cast<ControllerError>(ec.value());
        if (val >= ControllerError::TPS55289_None && val <= ControllerError::TPS55289_InvalidParameter) {
            return val;
        }
        if (val >= ControllerError::I2CArbitrationLost && val <= ControllerError::I2CBusError) {
            return val;
        }
    }
    return ControllerError::UnknownError;
}

ControllerError from_psram_error(const std::error_code& ec) {
    if (ec.category() != psram::get_error_category()) {
        return ControllerError::UnknownError;
    }
    switch (static_cast<psram::ErrorCode>(ec.value())) {
        case psram::ErrorCode::None:           return ControllerError::PSRAM_None;
        case psram::ErrorCode::HALError:       return ControllerError::PSRAM_HALError;
        case psram::ErrorCode::Timeout:        return ControllerError::PSRAM_Timeout;
        case psram::ErrorCode::InvalidAddress: return ControllerError::PSRAM_InvalidAddress;
        case psram::ErrorCode::InvalidSize:    return ControllerError::PSRAM_InvalidSize;
        case psram::ErrorCode::NotInitialized: return ControllerError::PSRAM_NotInitialized;
        case psram::ErrorCode::QSPIError:
        case psram::ErrorCode::DMAError:       return ControllerError::PSRAM_QSPIError;
        case psram::ErrorCode::ClockTooSlow:   return ControllerError::PSRAM_ClockTooSlow;
        case psram::ErrorCode::ClockTooFast:   return ControllerError::PSRAM_ClockTooFast;
        case psram::ErrorCode::ConfigError:    return ControllerError::PSRAM_ConfigError;
        case psram::ErrorCode::NotSupported:   return ControllerError::PSRAM_NotSupported;
        case psram::ErrorCode::IdMismatch:     return ControllerError::PSRAM_IdMismatch;
        case psram::ErrorCode::VerifyFailed:   return ControllerError::PSRAM_VerifyFailed;
        case psram::ErrorCode::Busy:           return ControllerError::PSRAM_Busy;
        default:                               return ControllerError::UnknownError;
    }
}

ControllerError from_mcp44xx_error(const std::error_code& ec) {
    if (ec.category() != mcp44xx::get_error_category()) {
        return ControllerError::UnknownError;
    }
    switch (static_cast<mcp44xx::ErrorCode>(ec.value())) {
        case mcp44xx::ErrorCode::None:             return ControllerError::MCP44XX_None;
        case mcp44xx::ErrorCode::HALError:         return ControllerError::MCP44XX_HALError;
        case mcp44xx::ErrorCode::Timeout:          return ControllerError::MCP44XX_Timeout;
        case mcp44xx::ErrorCode::InvalidAddress:
        case mcp44xx::ErrorCode::InvalidParameter: return ControllerError::MCP44XX_InvalidParameter;
        case mcp44xx::ErrorCode::InvalidWiper:     return ControllerError::MCP44XX_InvalidWiper;
        case mcp44xx::ErrorCode::InvalidSlot:      return ControllerError::MCP44XX_InvalidSlot;
        case mcp44xx::ErrorCode::EEPROMBusy:       return ControllerError::MCP44XX_EEPROMBusy;
        case mcp44xx::ErrorCode::I2CError:
        case mcp44xx::ErrorCode::Nack:             return ControllerError::I2CNACK;
        case mcp44xx::ErrorCode::BusError:         return ControllerError::I2CBusError;
        default:                                   return ControllerError::UnknownError;
    }
}

ControllerError from_dac_error(ControllerError ec) {
    // DAC errors are already in ControllerError enum
    if (ec >= ControllerError::DAC_None && ec <= ControllerError::DAC_WAVEFORM_ERROR) {
        return ec;
    }
    // Check if it's an oscilloscope or I2C error
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

ControllerError from_dac_error(const std::error_code& ec) {
    if (ec.category() == dac_error_category()) {
        return from_dac_error(static_cast<ControllerError>(ec.value()));
    }
    if (ec.category() == controller_error_category()) {
        auto val = static_cast<ControllerError>(ec.value());
        if (val >= ControllerError::DAC_None && val <= ControllerError::DAC_WAVEFORM_ERROR) {
            return val;
        }
        // Check for other controller-specific errors
        if (val >= ControllerError::OscilloscopeNotConfigured && val <= ControllerError::OscilloscopeOutOfRange) {
            return val;
        }
        if (val >= ControllerError::I2CArbitrationLost && val <= ControllerError::I2CBusError) {
            return val;
        }
        if (val >= ControllerError::MemoryAllocationFailed && val <= ControllerError::ADCBufferOverrun) {
            return val;
        }
    }
    return ControllerError::UnknownError;
}

// =============================================================================
// Helper Functions
// =============================================================================

const char* get_error_source(const std::error_code& ec) noexcept {
    const auto& cat = ec.category();
    if (cat == bq25792_error_category()) return "bq25792";
    if (cat == tps55289_error_category()) return "tps55289";
    if (cat == psram::get_error_category()) return "psram";
    if (cat == mcp44xx::get_error_category()) return "mcp44xx";
    if (is_dac_error(ec)) return "dac";
    if (is_hal_error(ec)) return "hal";
    return "controller";
}

bool is_bq25792_error(const std::error_code& ec) {
    if (ec.category() == bq25792_error_category()) {
        return true;
    }
    return ec.category() == controller_error_category() &&
           ec.value() >= static_cast<int>(ControllerError::BQ25792_None) &&
           ec.value() <= static_cast<int>(ControllerError::BQ25792_FaultDetected);
}

bool is_tps55289_error(const std::error_code& ec) {
    if (ec.category() == tps55289_error_category()) {
        return true;
    }
    return ec.category() == controller_error_category() &&
           ec.value() >= static_cast<int>(ControllerError::TPS55289_None) &&
           ec.value() <= static_cast<int>(ControllerError::TPS55289_InvalidParameter);
}

bool is_dac_error(const std::error_code& ec) {
    return ec.category() == dac_error_category() ||
           (ec.category() == controller_error_category() &&
            ec.value() >= static_cast<int>(ControllerError::DAC_None) &&
            ec.value() <= static_cast<int>(ControllerError::DAC_WAVEFORM_ERROR));
}

bool is_hal_error(const std::error_code& ec) {
    return ec.category() == controller_error_category() &&
           ec.value() >= static_cast<int>(ControllerError::HAL_OK) &&
           ec.value() <= static_cast<int>(ControllerError::HAL_TIMEOUT);
}

bool is_i2c_bus_fault(ControllerError ec) noexcept {
    switch (ec) {
        case ControllerError::I2CBusError:
        case ControllerError::I2CArbitrationLost:
        case ControllerError::HAL_TIMEOUT:
        case ControllerError::BQ25792_Timeout:
        case ControllerError::TPS55289_Timeout:
        case ControllerError::MCP44XX_Timeout:
            return true;
        default:
            return false;
    }
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
