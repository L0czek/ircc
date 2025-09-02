#include <cstdio>
#include <expected>

#include "ctl.h"

#include "board.h"
#include "cmsis_os2.h"
#include "config.hpp"
#include "stm32g474xx.h"
#include "stm32g4xx_hal_def.h"
#include "stm32g4xx_hal_i2c.h"
#include "stm32g4xx_hal_tim.h"
#include "stm32g4xx_hal_tim_ex.h"
#include "stm32g4xx_hal_i2c_ex.h"
#include "thread.hpp"
#include "TCA9535.hpp"


#include "log.hpp"

void ScanI2CDevices(I2C_HandleTypeDef *i2c)
{
    os::info("Starting I2C scan...\n");

    for (uint8_t address = 0x0; address <= 0x7f; ++address)
    {
        uint8_t i2c_address = address << 1;

        HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(i2c,  i2c_address, 1, 1000);

        if (st == HAL_OK)
        {
            os::info("Device found at address 0x%02X\n", address);
        }
    }

    os::info("I2C scan complete.\n");
}

extern "C" void controller_system_init(const board_config *config) {
    os::init(LOG_LEVEL);
    HAL_TIM_IC_Start_IT(config->feedback_input_timer, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(config->debug_timer, TIM_CHANNEL_1);
    os::info("Booting up\n");

    for (uint32_t i=0;; ++i) {
        os::info("tick %d\n", i);
        os::info("temp: %d\n", temp);
        osDelay(1000);
        /* HAL_StatusTypeDef st = HAL_I2C_IsDeviceReady(config->i2c, 0x50, 1, HAL_MAX_DELAY); */
        /* os::info("Check for sfp: %d\n", st); */
        ScanI2CDevices(config->i2c);
    }
}

