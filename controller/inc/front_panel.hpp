#ifndef __FRONT_PANEL_HPP__
#define __FRONT_PANEL_HPP__

#include "stm32g4xx_hal.h"
#include <cstdint>

class MessageHandler;

/**
 * @brief Front panel (resonance_converter_controller_io board): SSD1327 OLED on SPI3,
 *        PCF8574 keypad on the internal I2C bus (KEY_INT on PC5), encoder on TIM20.
 *        docs/lcd_ui.md.
 *
 * One thread ("panel", static memory, osPriorityBelowNormal) runs the UI from
 * controller/ui at 25 Hz: reads the keys and the encoder, ticks the UI, renders and sends
 * the changed rows to the panel (SPI DMA when CubeMX gives SPI3 a TX DMA channel), and
 * applies the display power policy (dim, off = RES# low which also turns the panel
 * supply off). A missing panel or keypad is retried every few seconds.
 *
 * The keypad address is a compile-time setting (CMake FRONT_PANEL_KEYPAD_I2C_ADDRESS).
 */
#ifndef FRONT_PANEL_KEYPAD_I2C_ADDRESS
#define FRONT_PANEL_KEYPAD_I2C_ADDRESS 0x20
#endif

namespace front_panel {

constexpr uint8_t kKeypadI2cAddress = FRONT_PANEL_KEYPAD_I2C_ADDRESS;
static_assert(kKeypadI2cAddress >= 0x20 && kKeypadI2cAddress <= 0x27, "PCF8574 address is 0x20..0x27");

/** @brief Start the panel thread (once); requests go through `handler` like host requests */
void start(MessageHandler& handler) noexcept;

/** @brief From HAL_GPIO_EXTI_Callback (KEY_INT falling edge) */
void on_key_interrupt() noexcept;

/** @brief From HAL_SPI_TxCpltCallback / HAL_SPI_ErrorCallback */
void on_spi_done(SPI_HandleTypeDef* hspi, bool ok) noexcept;

} // namespace front_panel

#endif
