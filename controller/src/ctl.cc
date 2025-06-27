#include <cstdio>

#include "ctl.h"

#include "board.h"
#include "cmsis_os2.h"
#include "config.hpp"
#include "stm32g4xx_hal_tim.h"
#include "stm32g4xx_hal_tim_ex.h"
#include "thread.hpp"

#include "log.hpp"

extern "C" void controller_system_init(const board_config *config) {
    os::init(LOG_LEVEL);
    HAL_TIM_IC_Start_IT(config->feedback_input_timer, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(config->debug_timer, TIM_CHANNEL_1);
    os::info("Booting up\n");

    for (uint32_t i=0;; ++i) {
        os::info("tick %d\n", i);
        os::info("temp: %d\n", temp);
        osDelay(1000);
    }
}
