#include "encoder.hpp"
#include "log.hpp"

Encoder::Encoder(TIM_HandleTypeDef* htim, const Config& config) noexcept
    : htim_(htim), config_(config) {}

std::expected<void, ControllerError> Encoder::start() noexcept {
    if (!htim_ || !htim_->Instance) {
        return std::unexpected(ControllerError::Peripheral_NotConfigured);
    }
    // SMS bits of SMCR hold the encoder mode set by HAL_TIM_Encoder_Init
    const uint32_t sms = htim_->Instance->SMCR & TIM_SMCR_SMS;
    const bool ti12 = sms == TIM_ENCODERMODE_TI12;
    const bool ti1_or_2 = sms == TIM_ENCODERMODE_TI1 || sms == TIM_ENCODERMODE_TI2;
    if (!ti12 && !ti1_or_2) {
        os::error("encoder: timer is not in a quadrature encoder mode (SMCR.SMS=0x%lx)\n",
                  static_cast<unsigned long>(sms));
        return std::unexpected(ControllerError::Peripheral_NotConfigured);
    }
    per_detent_ = config_.counts_per_detent > 0 ? config_.counts_per_detent : (ti12 ? 4 : 2);

    const HAL_StatusTypeDef st = HAL_TIM_Encoder_Start(htim_, TIM_CHANNEL_ALL);
    if (st != HAL_OK) {
        os::error("encoder: start failed (%d)\n", static_cast<int>(st));
        return std::unexpected(from_hal_status(st));
    }
    last_cnt_ = htim_->Instance->CNT;
    counts_ = 0;
    reported_ = 0;
    started_ = true;
    return {};
}

void Encoder::stop() noexcept {
    if (started_) {
        (void)HAL_TIM_Encoder_Stop(htim_, TIM_CHANNEL_ALL);
    }
    started_ = false;
}

void Encoder::update() noexcept {
    if (!started_) {
        return;
    }
    // Movement modulo the counter range (ARR + 1), taken as the shortest way round
    const uint32_t cnt = htim_->Instance->CNT;
    const uint64_t range = static_cast<uint64_t>(htim_->Instance->ARR) + 1u;
    int64_t diff = static_cast<int64_t>((static_cast<uint64_t>(cnt) + range - last_cnt_) % range);
    if (diff > static_cast<int64_t>(range / 2u)) {
        diff -= static_cast<int64_t>(range);
    }
    last_cnt_ = cnt;
    counts_ += config_.reverse ? -diff : diff;
}

int32_t Encoder::floor_div(int64_t a, int32_t b) noexcept {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return static_cast<int32_t>(q);
}

int32_t Encoder::position() noexcept {
    update();
    return floor_div(counts_, per_detent_);
}

int32_t Encoder::take_delta() noexcept {
    const int32_t pos = position();
    const int32_t delta = pos - reported_;
    reported_ = pos;
    return delta;
}

void Encoder::reset(int32_t position) noexcept {
    update();
    // Keep the sub-detent remainder so the next detent is not cut short
    const int64_t remainder = counts_ - static_cast<int64_t>(floor_div(counts_, per_detent_)) * per_detent_;
    counts_ = static_cast<int64_t>(position) * per_detent_ + remainder;
    reported_ = position;
}
