#include "error.hpp"
#include "stm32g4xx_hal_def.h"
#include <system_error>


const char *ControllerErrorCategory::name() const noexcept {
    return "Controller error";
}

std::string ControllerErrorCategory::message(int error) const {
    switch (static_cast<ControllerError>(error)) {
        case ControllerError::HAL_Status_Error:
            return "HAL status error";
        case ControllerError::HAL_Status_Busy:
            return "HAL status busy";
        case ControllerError::HAL_Status_Timeout:
            return "HAL status timeout";
        case ControllerError::HAL_Status_Ok:
            return "HAL status ok";
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

ControllerError from_hal_status(HAL_StatusTypeDef status) {
    switch (status) {
        case HAL_OK:      return ControllerError::HAL_Status_Ok;
        case HAL_ERROR:   return ControllerError::HAL_Status_Error;
        case HAL_BUSY:    return ControllerError::HAL_Status_Busy;
        case HAL_TIMEOUT: return ControllerError::HAL_Status_Timeout;
        default: return ControllerError::UnknownError;
    }
}
