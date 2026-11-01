#ifndef __BOARD_IO_HPP__
#define __BOARD_IO_HPP__

#include "encoder.hpp"
#include "fan.hpp"
#include "ws2812b.hpp"

/**
 * @brief Board user-interface / housekeeping peripherals, bound to the g_board_* handles.
 *
 * Accessors return nullptr on boards that lack the peripheral (Nucleo). The objects are
 * created on first use; start_io() starts the encoder and turns the LEDs off. The fan is
 * left to its owner (thermal policy): call fan()->start() and set_duty().
 */
namespace board {

/** @brief Number of WS2812B LEDs on the chain (one on the controller board) */
constexpr size_t kLedCount = 1;

Fan* fan() noexcept;
Encoder* encoder() noexcept;
Ws2812b* leds() noexcept;

/** @brief Start the encoder and the LED chain (all off); failures are logged */
void start_io() noexcept;

} // namespace board

#endif
