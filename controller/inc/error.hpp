#ifndef __ERROR_HPP__
#define __ERROR_HPP__

#include "stm32g4xx_hal_def.h"
#include <expected>
#include <system_error>
#include <type_traits>

enum class ControllerError {
    HAL_Status_Ok,
    HAL_Status_Error,
    HAL_Status_Busy,
    HAL_Status_Timeout,

    UnknownError,
};

class ControllerErrorCategory :public std::error_category {
public:
    const char * name() const noexcept override;
    std::string message(int ) const override;
};

const std::error_category& controller_error_category();
std::error_condition make_error_condition(ControllerError );

namespace std {
    template <>
    struct is_error_condition_enum<ControllerError>
        :public std::true_type {};
}

ControllerError from_hal_status(HAL_StatusTypeDef );

template<typename T>
std::expected<T, ControllerError> hal_error(HAL_StatusTypeDef status) {
    return std::unexpected(from_hal_status(status));
}

#endif
