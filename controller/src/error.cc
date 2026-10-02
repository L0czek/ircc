#include "error.hpp"
#include "stm32g4xx_hal.h"
#include <system_error>
#include <cstring>

// =============================================================================
// ControllerErrorCategory Implementation
// =============================================================================

const char* ControllerErrorCategory::name() const noexcept {
    return "controller";
}

std::string ControllerErrorCategory::message(int error) const {
    switch (static_cast<ControllerError>(error)) {
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
        
        case ControllerError::UnknownError:
        default:
            return "Unknown error";
    }
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
// BQ25792ErrorCategory Implementation
// =============================================================================

const char* BQ25792ErrorCategory::name() const noexcept {
    return "bq25792";
}

std::string BQ25792ErrorCategory::message(int error) const {
    switch (static_cast<bq25792::ErrorCode>(error)) {
        case bq25792::ErrorCode::None:
            return "BQ25792 no error";
        case bq25792::ErrorCode::HALError:
            return "BQ25792 HAL error";
        case bq25792::ErrorCode::Timeout:
            return "BQ25792 operation timeout";
        case bq25792::ErrorCode::I2CError:
            return "BQ25792 I2C communication error";
        case bq25792::ErrorCode::InvalidAddress:
            return "BQ25792 invalid register address";
        case bq25792::ErrorCode::InvalidParameter:
            return "BQ25792 invalid parameter";
        case bq25792::ErrorCode::NotInitialized:
            return "BQ25792 not initialized";
        case bq25792::ErrorCode::BatteryNotPresent:
            return "BQ25792 battery not present";
        case bq25792::ErrorCode::FaultDetected:
            return "BQ25792 fault condition detected";
        default:
            return "BQ25792 unknown error";
    }
}

const std::error_category& bq25792_error_category() {
    static BQ25792ErrorCategory category;
    return category;
}

// =============================================================================
// TPS55289ErrorCategory Implementation
// =============================================================================

const char* TPS55289ErrorCategory::name() const noexcept {
    return "tps55289";
}

std::string TPS55289ErrorCategory::message(int error) const {
    switch (static_cast<tps55289::ErrorCode>(error)) {
        case tps55289::ErrorCode::None:
            return "TPS55289 no error";
        case tps55289::ErrorCode::HALError:
            return "TPS55289 HAL error";
        case tps55289::ErrorCode::Timeout:
            return "TPS55289 operation timeout";
        case tps55289::ErrorCode::InvalidAddress:
            return "TPS55289 invalid I2C address";
        case tps55289::ErrorCode::InvalidRegister:
            return "TPS55289 invalid register address";
        case tps55289::ErrorCode::NotInitialized:
            return "TPS55289 not initialized";
        case tps55289::ErrorCode::I2CError:
            return "TPS55289 I2C communication error";
        default:
            return "TPS55289 unknown error";
    }
}

const std::error_category& tps55289_error_category() {
    static TPS55289ErrorCategory category;
    return category;
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
        if (val >= ControllerError::TPS55289_None && val <= ControllerError::TPS55289_I2CError) {
            return val;
        }
    }
    return ControllerError::UnknownError;
}

ControllerError from_dac_error(ControllerError ec) {
    // DAC errors are already in ControllerError enum
    if (ec >= ControllerError::DAC_None && ec <= ControllerError::DAC_WAVEFORM_ERROR) {
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
    }
    return ControllerError::UnknownError;
}

// =============================================================================
// Helper Functions
// =============================================================================

std::string get_error_source(const std::error_code& ec) {
    if (is_bq25792_error(ec)) {
        return "bq25792";
    } else if (is_tps55289_error(ec)) {
        return "tps55289";
    } else if (is_dac_error(ec)) {
        return "dac";
    } else if (is_hal_error(ec)) {
        return "hal";
    } else {
        return "controller";
    }
}

bool is_bq25792_error(const std::error_code& ec) {
    return ec.category() == bq25792_error_category() ||
           ec.category() == controller_error_category();
}

bool is_tps55289_error(const std::error_code& ec) {
    return ec.category() == tps55289_error_category() ||
           ec.category() == controller_error_category();
}

bool is_dac_error(const std::error_code& ec) {
    return ec.category() == dac_error_category() ||
           (ec.category() == controller_error_category() &&
            ec.value() >= static_cast<int>(ControllerError::DAC_None) &&
            ec.value() <= static_cast<int>(ControllerError::DAC_WAVEFORM_ERROR));
}

bool is_hal_error(const std::error_code& ec) {
    // HAL errors are mapped to ControllerError enum
    auto val = static_cast<ControllerError>(ec.value());
    return val >= ControllerError::HAL_OK && val <= ControllerError::HAL_TIMEOUT;
}
