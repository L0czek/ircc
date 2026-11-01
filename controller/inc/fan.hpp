#ifndef __FAN_HPP__
#define __FAN_HPP__

#include "error.hpp"
#include "stm32g4xx_hal.h"
#include <atomic>
#include <cstdint>
#include <expected>

/**
 * @brief 4-pin PC fan driven by a timer PWM channel (FAN_PWM).
 *
 * Board: g_board_tim_fan_pwm / g_board_tim_fan_pwm_channel (TIM3_CH1, PA6), routed to
 * the PWM pin of the dock's 4-pin fan header. The tach pin is not connected to the MCU,
 * so the speed is open loop.
 *
 * The CubeMX init configures the channel in PWM1 mode; start() sets the PWM frequency
 * (25 kHz per the 4-wire fan spec) by reprogramming PSC/ARR, so the CubeMX period value
 * does not matter. Before start() the pin is whatever the CubeMX init left (CCR = 0, timer
 * stopped); 4-wire fans pull their PWM input up and run at full speed if it floats.
 *
 * set_duty() is a single CCR write and may be called from any thread.
 */
class Fan {
public:
    struct Config {
        uint32_t pwm_hz = 25000;
        float min_duty = 0.0f;      // non-zero requests are raised to this (fans that stall)
        bool inverted = false;      // true if an inverting stage sits between pin and fan
    };

    Fan(TIM_HandleTypeDef* htim, uint32_t channel) noexcept : Fan(htim, channel, Config{}) {}
    Fan(TIM_HandleTypeDef* htim, uint32_t channel, const Config& config) noexcept;

    /** @brief Program the PWM frequency and start the output at the current duty (0 % after boot) */
    std::expected<void, ControllerError> start() noexcept;

    /** @brief Drive 0 % duty and disable the channel */
    void stop() noexcept;

    /** @brief Duty in percent, 0..100; values below min_duty (except 0) are raised to it */
    std::expected<void, ControllerError> set_duty(float percent) noexcept;

    float duty() const noexcept { return duty_.load(std::memory_order_relaxed); }
    bool running() const noexcept { return running_; }
    uint32_t frequency_hz() const noexcept { return actual_hz_; }

private:
    void write_compare(float percent) noexcept;

    TIM_HandleTypeDef* htim_;
    uint32_t channel_;
    Config config_;
    std::atomic<float> duty_{0.0f};
    uint32_t actual_hz_ = 0;
    bool running_ = false;
};

#endif
