#ifndef __ENCODER_HPP__
#define __ENCODER_HPP__

#include "error.hpp"
#include "stm32g4xx_hal.h"
#include <cstdint>
#include <expected>

/**
 * @brief Quadrature rotary encoder on a timer in encoder mode.
 *
 * Board: g_board_tim_encoder (TIM20, CH1 = PB2, CH2 = PE3), wired straight to the IO
 * board encoder (J3.14 / J3.15, 10 k pull-ups and 100 nF on the IO board). The push switch
 * is not on the timer: it is read through the IO board's PCF8574 (KEY_INT). The front
 * panel thread is the only user.
 *
 * The hardware counter does the counting; update() extends it to 32 bits and must run
 * more often than half a counter wrap (32768 counts, i.e. any UI polling rate). The
 * counts per detent depend on the CubeMX encoder mode: TI1 or TI2 counts 2 edges per
 * quadrature cycle, TI12 counts 4; a typical mechanical encoder has one quadrature
 * cycle per detent.
 *
 * Not thread safe: use one instance from one thread (the UI thread).
 */
class Encoder {
public:
    struct Config {
        int32_t counts_per_detent = 0;   // 0: derive from the encoder mode (2 for TI1/TI2, 4 for TI12)
        bool reverse = false;            // swap the direction
    };

    explicit Encoder(TIM_HandleTypeDef* htim) noexcept : Encoder(htim, Config{}) {}
    Encoder(TIM_HandleTypeDef* htim, const Config& config) noexcept;

    /** @brief Start the counter (HAL_TIM_Encoder_Start); position starts at 0 */
    std::expected<void, ControllerError> start() noexcept;
    void stop() noexcept;

    /** @brief Read the hardware counter and accumulate the movement since the last call */
    void update() noexcept;

    /** @brief Absolute position in detents (calls update()) */
    int32_t position() noexcept;

    /** @brief Detents moved since the previous take_delta() (calls update()) */
    int32_t take_delta() noexcept;

    /** @brief Set the current position (in detents) */
    void reset(int32_t position = 0) noexcept;

    /** @brief Raw counts since start (no detent scaling) */
    int64_t counts() const noexcept { return counts_; }

private:
    static int32_t floor_div(int64_t a, int32_t b) noexcept;

    TIM_HandleTypeDef* htim_;
    Config config_;
    int32_t per_detent_ = 2;
    uint32_t last_cnt_ = 0;
    int64_t counts_ = 0;
    int32_t reported_ = 0;      // position at the last take_delta()
    bool started_ = false;
};

#endif
