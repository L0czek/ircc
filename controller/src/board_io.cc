#include "board_io.hpp"
#include "board.h"
#include "log.hpp"

namespace board {

#if defined(resonant_converter_controller)

Fan* fan() noexcept {
    static Fan instance(g_board_tim_fan_pwm, g_board_tim_fan_pwm_channel);
    return &instance;
}

Encoder* encoder() noexcept {
    static Encoder instance(g_board_tim_encoder);
    return &instance;
}

Ws2812b* leds() noexcept {
    static Ws2812b::Storage<kLedCount> storage;
    static Ws2812b instance(g_board_tim_ws2812b, g_board_tim_ws2812b_channel, storage);
    return &instance;
}

void start_io() noexcept {
    auto enc = encoder()->start();
    if (!enc) {
        os::error("encoder: %s\n", to_string(enc.error()));
    }
    auto led = leds()->start();
    if (!led) {
        os::error("ws2812b: %s\n", to_string(led.error()));
    }
}

#else

Fan* fan() noexcept { return nullptr; }
Encoder* encoder() noexcept { return nullptr; }
Ws2812b* leds() noexcept { return nullptr; }
void start_io() noexcept {}

#endif

} // namespace board
