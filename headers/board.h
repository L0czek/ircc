#ifndef __BOARD_H__
#define __BOARD_H__

#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_dac.h"
#include "stm32g4xx_hal_adc.h"
#include "stm32g4xx_hal_crc.h"
#include "stm32g4xx_hal_dma.h"
#include "stm32g4xx_hal_i2c.h"
#include "stm32g4xx_hal_uart.h"
#include "stm32g4xx_hal_tim.h"
#include "stm32g4xx_hal_hrtim.h"
#include "stm32g4xx_hal_spi.h"
#include "stm32g4xx_hal_qspi.h"

#ifdef __cplusplus

extern "C" {

#endif

typedef struct {
    GPIO_TypeDef* GPIOx;
    uint32_t pin;
} GPIO_pin_t;

typedef struct {
    DAC_HandleTypeDef *DACx;
    uint32_t channel;
} DAC_channel_t;

typedef struct {
    ADC_HandleTypeDef *ADCx;
    uint32_t channel;
} ADC_channel_t;

// Board peripheral externs - names imported from target/resonant_converter_controller/Core/Src/main.c
#if defined(stm32g474re_nucleo_board)
// Nucleo board - use g_board_* naming directly
extern TIM_HandleTypeDef *g_board_tim_feedback;
extern TIM_HandleTypeDef *g_board_tim_debug;
extern HRTIM_HandleTypeDef *g_board_hrtim;
extern I2C_HandleTypeDef *g_board_i2c_internal;
extern UART_HandleTypeDef *g_board_ctl_uart;
extern DMA_HandleTypeDef *g_board_dma_uart_rx;
extern DMA_HandleTypeDef *g_board_dma_uart_tx;
extern CRC_HandleTypeDef *g_board_crc32;
extern DAC_HandleTypeDef *g_board_dac_ch1;
extern DAC_HandleTypeDef *g_board_dac_ch2;
extern DAC_HandleTypeDef *g_board_dac_ch3;
extern DAC_HandleTypeDef *g_board_dac_ch4;
extern DAC_HandleTypeDef *g_board_dac_channels[4];
// Channel aliases for aux_pwm timer (TIM_DEBUG)
#define g_board_aux_pwm g_board_tim_debug
#define g_board_aux_pwm_cha TIM_CHANNEL_1
#define g_board_aux_pwm_chb TIM_CHANNEL_2
#elif defined(resonant_converter_controller)
// Resonant converter controller - use g_board_* naming from main.c
extern HRTIM_HandleTypeDef *g_board_hrtim;
extern TIM_HandleTypeDef *g_board_tim_feedback;
extern TIM_HandleTypeDef *g_board_aux_pwm;
extern const uint32_t g_board_aux_pwm_cha;
extern const uint32_t g_board_aux_pwm_chb;
extern TIM_HandleTypeDef *g_board_tim_fan_pwm;
extern TIM_HandleTypeDef *g_board_tim_ws2812b;
extern TIM_HandleTypeDef *g_board_tim_encoder;
extern I2C_HandleTypeDef *g_board_i2c_internal;
extern I2C_HandleTypeDef *g_board_i2c_external;
extern SPI_HandleTypeDef *g_board_lcd_spi;
extern SPI_HandleTypeDef *g_board_sdcard_spi;
extern QSPI_HandleTypeDef *g_board_psram_qspi;
extern UART_HandleTypeDef *g_board_ctl_uart;
extern UART_HandleTypeDef *g_board_aux_uart;
extern CRC_HandleTypeDef *g_board_crc32;
extern DAC_HandleTypeDef *g_board_dac_ch1;
extern DAC_HandleTypeDef *g_board_dac_ch2;
extern DAC_HandleTypeDef *g_board_dac_ch3;
extern DAC_HandleTypeDef *g_board_dac_ch4;
extern DAC_HandleTypeDef *g_board_dac_channels[4];
#endif

// BOARD_CONFIG struct for use in C++ code
// This must be defined AFTER the extern declarations so we can reference HAL types
struct BoardConfig {
    UART_HandleTypeDef* message_bus;
    CRC_HandleTypeDef* crc32;
};

// BOARD_CONFIG instance - defined in main.c
#if defined(stm32g474re_nucleo_board) || defined(resonant_converter_controller)
extern BoardConfig g_board_config;
#define BOARD_CONFIG g_board_config
#endif

void controller_system_init(void);

#ifdef __cplusplus

}

#endif

#endif
