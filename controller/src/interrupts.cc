#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_tim.h"
#include "ws2812b.hpp"
#include <cstdint>

volatile uint32_t temp;

extern "C" void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
    temp = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_1);
}

// End of a timer PWM DMA transfer (WS2812B frames)
extern "C" void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim) {
    Ws2812b::on_pulse_finished(htim);
}
