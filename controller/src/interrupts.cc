#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_tim.h"
#include "board.h"
#include "front_panel.hpp"
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

// KEY_INT (PC5, PCF8574 ~INT): active once CubeMX configures PC5 as EXTI falling edge;
// until then the panel polls the keypad every frame.
extern "C" void HAL_GPIO_EXTI_Callback(uint16_t pin) {
#if defined(resonant_converter_controller)
    if (pin == g_board_key_int.pin) {
        front_panel::on_key_interrupt();
    }
#else
    (void)pin;
#endif
}

// SPI DMA transfers (SSD1327 OLED on SPI3)
extern "C" void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
    front_panel::on_spi_done(hspi, true);
}

extern "C" void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi) {
    front_panel::on_spi_done(hspi, false);
}
