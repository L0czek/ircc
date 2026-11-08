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
extern TIM_HandleTypeDef *g_board_tim_adc_trgo;      // oscilloscope sample timer (TIM8)
extern UART_HandleTypeDef *g_board_ctl_uart;
extern UART_HandleTypeDef *g_board_aux_uart;
extern CRC_HandleTypeDef *g_board_crc32;
extern DAC_HandleTypeDef *g_board_dac_ch1;
extern DAC_HandleTypeDef *g_board_dac_ch2;
extern DAC_HandleTypeDef *g_board_dac_ch3;
extern DAC_HandleTypeDef *g_board_dac_ch4;
extern DAC_HandleTypeDef *g_board_dac_channels[4];

// Timer channels / HRTIM resources (defined next to the handles in main.c)
extern const uint32_t g_board_hrtim_out1;
extern const uint32_t g_board_hrtim_out2;
extern const uint32_t g_board_hrtim_out3;
extern const uint32_t g_board_hrtim_out4;
extern const uint32_t g_board_hrtim_sync_out1_p;
extern const uint32_t g_board_hrtim_sync_out1_n;
extern const uint32_t g_board_hrtim_sync_out2_p;
extern const uint32_t g_board_hrtim_sync_out2_n;
extern const uint32_t g_board_hrtim_sync_out3_p;
extern const uint32_t g_board_hrtim_sync_out3_n;
extern const uint32_t g_board_hrtim_delayd_eev1;
extern const uint32_t g_board_hrtim_flt;
extern const uint32_t g_board_tim_feedback_channel;
extern const uint32_t g_board_tim_fan_pwm_channel;      // TIM3_CH1, PA6 -> FAN_PWM (dock 4-pin fan)
extern const uint32_t g_board_tim_ws2812b_channel;      // TIM5_CH4, PE9 -> WS2812B DIN

// Analog front end
extern DAC_channel_t g_board_fb_sync_v;
extern DAC_channel_t g_board_dac_aux_v;
extern DAC_channel_t g_board_fb_ocd_comp_v;
extern DAC_channel_t g_board_fb_ocd2_comp_v;
extern DAC_channel_t g_board_isense_comp_v[4];
extern ADC_channel_t g_board_aux_adc_read_v;
extern ADC_channel_t g_board_dac_aux_read_back_v;
extern ADC_channel_t g_board_fb_ocd_read_v;
extern ADC_channel_t g_board_fb_ocd2_read_v;
extern ADC_channel_t g_board_vsense_read_v;
extern COMP_HandleTypeDef *g_board_isense[4];
extern COMP_HandleTypeDef *g_board_fb_ocd;
extern COMP_HandleTypeDef *g_board_fb_ocd2;

// GPIOs
extern GPIO_pin_t g_board_gpios[8];
extern GPIO_pin_t g_board_power_int;
extern GPIO_pin_t g_board_vout_enable;
extern GPIO_pin_t g_board_sdcard_detect;
extern GPIO_pin_t g_board_mco_nrst_sel;
extern GPIO_pin_t g_board_usb_dap_en;
extern GPIO_pin_t g_board_usb_dap_dir;
extern GPIO_pin_t g_board_lcd_bl;
extern GPIO_pin_t g_board_lcd_dc;
extern GPIO_pin_t g_board_lcd_rst;
extern GPIO_pin_t g_board_lcd_cs;     // PA15 LCD_SS (SSD1327 CS#)
extern GPIO_pin_t g_board_key_int;
// PD0 = USB_DEAD_BATT_DISABLE (high disconnects the external dead-battery Rd on CC1/CC2);
// g_board_lcd_bl is the same pin under its stale CubeMX name.
extern GPIO_pin_t g_board_usb_dead_batt_disable;
// PC13 = BRIDGE_VOUT_VBUS_USB: high connects VOUT to the USB-C VBUS (U26 eFuse)
extern GPIO_pin_t g_board_usb_vout_bridge;
#endif

// BOARD_CONFIG struct for use in C++ code
// This must be defined AFTER the extern declarations so we can reference HAL types
struct BoardConfig {
    UART_HandleTypeDef* message_bus;
    CRC_HandleTypeDef* crc32;
};

// BOARD_CONFIG instance - defined in main.c
#if defined(stm32g474re_nucleo_board) || defined(resonant_converter_controller)
extern struct BoardConfig g_board_config;
#define BOARD_CONFIG g_board_config
#endif

void controller_system_init(void);

#ifdef __cplusplus

}

#endif

#endif
