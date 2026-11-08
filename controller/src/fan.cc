#include "fan.hpp"
#include "board_timer.hpp"
#include "log.hpp"

Fan::Fan(TIM_HandleTypeDef* htim, uint32_t channel, const Config& config) noexcept
    : htim_(htim), channel_(channel), config_(config) {}

std::expected<void, ControllerError> Fan::start() noexcept {
    if (!htim_ || !htim_->Instance) {
        return std::unexpected(ControllerError::Peripheral_NotConfigured);
    }
    board_timer::Timing timing{};
    if (!board_timer::plan(htim_->Instance, config_.pwm_hz, timing)) {
        os::error("fan: cannot run the timer at %lu Hz\n", static_cast<unsigned long>(config_.pwm_hz));
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    if (running_) {
        stop();
    }
    board_timer::apply(htim_, timing);
    actual_hz_ = timing.actual_hz;
    write_compare(duty());
    const HAL_StatusTypeDef st = HAL_TIM_PWM_Start(htim_, channel_);
    if (st != HAL_OK) {
        os::error("fan: PWM start failed (%d)\n", static_cast<int>(st));
        return std::unexpected(from_hal_status(st));
    }
    running_ = true;
    os::info("fan: PWM %lu Hz, duty %d %%\n", static_cast<unsigned long>(actual_hz_), static_cast<int>(duty()));
    return {};
}

void Fan::stop() noexcept {
    if (htim_ && running_) {
        write_compare(0.0f);
        (void)HAL_TIM_PWM_Stop(htim_, channel_);
    }
    running_ = false;
}

std::expected<void, ControllerError> Fan::set_duty(float percent) noexcept {
    if (!(percent >= 0.0f && percent <= 100.0f)) {   // also rejects NaN
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    if (percent > 0.0f && percent < config_.min_duty) {
        percent = config_.min_duty;
    }
    duty_.store(percent, std::memory_order_relaxed);
    if (htim_ && htim_->Instance) {
        write_compare(percent);
    }
    return {};
}

void Fan::write_compare(float percent) noexcept {
    if (config_.inverted) {
        percent = 100.0f - percent;
    }
    // PWM1: high while CNT < CCR; CCR = ARR + 1 gives 100 %
    const uint32_t period = htim_->Instance->ARR + 1u;
    const uint32_t ccr = static_cast<uint32_t>(static_cast<float>(period) * percent / 100.0f + 0.5f);
    __HAL_TIM_SET_COMPARE(htim_, channel_, ccr);
}
