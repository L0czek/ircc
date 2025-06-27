#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_tim.h"
#include <cstdint>

volatile uint32_t temp;

extern "C" void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
    temp = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
}
