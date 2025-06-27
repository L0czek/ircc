#include "stm32g4xx_hal.h"

#ifdef __cplusplus

extern "C" {

#endif

    typedef struct {
        TIM_HandleTypeDef *feedback_input_timer;
        TIM_HandleTypeDef *debug_timer;
    } board_config;

    void controller_system_init(const board_config *config);

#ifdef __cplusplus

}

#endif
