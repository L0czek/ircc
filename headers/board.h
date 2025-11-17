#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_crc.h"
#include "stm32g4xx_hal_dma.h"
#include "stm32g4xx_hal_i2c.h"
#include "stm32g4xx_hal_uart.h"

#ifdef __cplusplus

extern "C" {

#endif

    typedef struct {
        TIM_HandleTypeDef *feedback_input_timer;
        TIM_HandleTypeDef *debug_timer;
        HRTIM_HandleTypeDef *high_res_timer;
        I2C_HandleTypeDef *i2c;
        UART_HandleTypeDef *message_bus;
        DMA_HandleTypeDef *message_bus_dma_rx;
        DMA_HandleTypeDef *message_bus_dma_tx;
        CRC_HandleTypeDef *crc32;
    } board_config;

    extern board_config BOARD_CONFIG;

    void controller_system_init(const board_config *config);

#ifdef __cplusplus

}

#endif
