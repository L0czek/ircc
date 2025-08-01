#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_i2c.h"

#ifdef __cplusplus

extern "C" {

#endif

    typedef struct {
        TIM_HandleTypeDef *feedback_input_timer;
        TIM_HandleTypeDef *debug_timer;
        HRTIM_HandleTypeDef *high_res_timer;
        I2C_HandleTypeDef *i2c;

    } board_config;

    void controller_system_init(const board_config *config);

#ifdef __cplusplus

}

#endif
