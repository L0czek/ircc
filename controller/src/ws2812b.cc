#include "ws2812b.hpp"
#include "board_timer.hpp"
#include "cmsis_os2.h"
#include "log.hpp"
#include <algorithm>

Ws2812b* Ws2812b::instances_[kMaxInstances] = {};

Ws2812b::Ws2812b(TIM_HandleTypeDef* htim, uint32_t channel, Rgb* pixels, uint32_t* dma_buffer, size_t count) noexcept
    : htim_(htim), channel_(channel), pixels_(pixels), dma_(dma_buffer), count_(count) {
    std::fill(pixels_, pixels_ + count_, Rgb{0, 0, 0});
}

std::expected<void, ControllerError> Ws2812b::start() noexcept {
    if (!htim_ || !htim_->Instance || !pixels_ || !dma_ || count_ == 0 || buffer_words(count_) > 0xFFFFu) {
        return std::unexpected(ControllerError::Peripheral_NotConfigured);
    }

    board_timer::Timing timing{};
    if (!board_timer::plan(htim_->Instance, kBitHz, timing) || timing.prescaler != 0) {
        os::error("ws2812b: timer clock too slow for 800 kHz bits\n");
        return std::unexpected(ControllerError::Peripheral_InvalidParameter);
    }
    const uint32_t f_tim = board_timer::clock_hz(htim_->Instance);
    t0_ = static_cast<uint32_t>((static_cast<uint64_t>(f_tim) * kT0HighNs + 500000000u) / 1000000000u);
    t1_ = static_cast<uint32_t>((static_cast<uint64_t>(f_tim) * kT1HighNs + 500000000u) / 1000000000u);

    // DMA link from CubeMX (TIMx_CHy request); without one, fall back to polled output.
    const DMA_HandleTypeDef* hdma = htim_->hdma[board_timer::dma_id(channel_)];
    use_dma_ = false;
    if (hdma && hdma->Instance) {
        const bool word_periph = hdma->Init.PeriphDataAlignment == DMA_PDATAALIGN_WORD;
        const bool mem_ok = hdma->Init.MemDataAlignment == DMA_MDATAALIGN_WORD ||
                            hdma->Init.MemDataAlignment == DMA_MDATAALIGN_HALFWORD;
        // A half-word write to a 32-bit CCR is replicated into both halves by the APB bridge
        if (!mem_ok || (board_timer::is_32bit(htim_->Instance) && !word_periph) ||
            hdma->Init.Direction != DMA_MEMORY_TO_PERIPH || hdma->Init.Mode != DMA_NORMAL) {
            os::error("ws2812b: DMA must be memory->peripheral, normal mode, peripheral word, "
                      "memory word or half-word\n");
            return std::unexpected(ControllerError::Peripheral_NotConfigured);
        }
        halfword_ = hdma->Init.MemDataAlignment == DMA_MDATAALIGN_HALFWORD;
        use_dma_ = true;
    } else if (count_ > kMaxPolledLeds) {
        os::error("ws2812b: %u LEDs need a DMA channel on the timer (polled limit %u)\n",
                  static_cast<unsigned>(count_), static_cast<unsigned>(kMaxPolledLeds));
        return std::unexpected(ControllerError::Peripheral_NotConfigured);
    }

    if (!started_) {
        size_t slot = 0;
        for (; slot < kMaxInstances && instances_[slot]; ++slot) {
        }
        if (slot == kMaxInstances) {
            return std::unexpected(ControllerError::Peripheral_Busy);
        }
        instances_[slot] = this;
    }

    // Idle: counter running at the bit rate, CCR = 0 (line low), output enabled. The HAL
    // channel state stays READY so HAL_TIM_PWM_Start_DMA can be used for each frame.
    htim_->Instance->CR1 &= ~TIM_CR1_CEN;
    __HAL_TIM_SET_COMPARE(htim_, channel_, 0);
    board_timer::apply(htim_, timing);
    TIM_CCxChannelCmd(htim_->Instance, channel_, TIM_CCx_ENABLE);
    __HAL_TIM_ENABLE(htim_);
    started_ = true;
    done_ms_.store(HAL_GetTick(), std::memory_order_relaxed);

    os::info("ws2812b: %u LEDs, %s output, T0H %lu / T1H %lu / period %lu ticks\n",
             static_cast<unsigned>(count_), use_dma_ ? "DMA" : "polled", static_cast<unsigned long>(t0_),
             static_cast<unsigned long>(t1_), static_cast<unsigned long>(timing.period + 1u));
    return show();   // all LEDs off (they power up with random colours on some batches)
}

void Ws2812b::set(size_t index, Rgb color) noexcept {
    if (index < count_) {
        pixels_[index] = color;
    }
}

void Ws2812b::fill(Rgb color) noexcept {
    std::fill(pixels_, pixels_ + count_, color);
}

void Ws2812b::encode() noexcept {
    // Wire order is G, R, B, MSB first; trailing slots stay low
    uint16_t* half = reinterpret_cast<uint16_t*>(dma_);
    size_t n = 0;
    auto put = [&](uint32_t v) {
        if (halfword_) {
            half[n++] = static_cast<uint16_t>(v);
        } else {
            dma_[n++] = v;
        }
    };
    for (size_t i = 0; i < count_; ++i) {
        const uint8_t bytes[3] = {scale(pixels_[i].g, brightness_), scale(pixels_[i].r, brightness_),
                                  scale(pixels_[i].b, brightness_)};
        for (uint8_t byte : bytes) {
            for (int bit = 7; bit >= 0; --bit) {
                put((byte >> bit) & 1u ? t1_ : t0_);
            }
        }
    }
    for (size_t i = 0; i < kTrailingSlots; ++i) {
        put(0);
    }
}

void Ws2812b::wait_latch() const noexcept {
    // Tick resolution is 1 ms: a difference of 2 guarantees > 1 ms (> kResetUs) of low line
    const uint32_t done = done_ms_.load(std::memory_order_relaxed);
    while (HAL_GetTick() - done < 2u) {
        if (osKernelGetState() == osKernelRunning) {
            osDelay(1);
        }
    }
}

std::expected<void, ControllerError> Ws2812b::show() noexcept {
    if (!started_) {
        return std::unexpected(ControllerError::Peripheral_NotStarted);
    }
    if (busy()) {
        return std::unexpected(ControllerError::Peripheral_Busy);
    }
    wait_latch();
    encode();

    if (!use_dma_) {
        send_polled();
        done_ms_.store(HAL_GetTick(), std::memory_order_relaxed);
        return {};
    }

    busy_.store(true, std::memory_order_release);
    const HAL_StatusTypeDef st = HAL_TIM_PWM_Start_DMA(htim_, channel_, dma_,
                                                       static_cast<uint16_t>(buffer_words(count_)));
    if (st != HAL_OK) {
        busy_.store(false, std::memory_order_release);
        return std::unexpected(from_hal_status(st));
    }
    return {};
}

void Ws2812b::send_polled() noexcept {
    TIM_TypeDef* t = htim_->Instance;
    const size_t bits = count_ * 24u;
    auto value = [&](size_t i) -> uint32_t {
        return halfword_ ? reinterpret_cast<const uint16_t*>(dma_)[i] : dma_[i];
    };

    board_timer::IrqLock lock;   // a missed update repeats a bit and corrupts the frame
    t->CR1 &= ~TIM_CR1_CEN;
    t->CNT = 0;
    __HAL_TIM_SET_COMPARE(htim_, channel_, value(0));
    t->EGR = TIM_EGR_UG;          // bit 0 becomes active now
    t->SR = ~TIM_SR_UIF;
    t->CR1 |= TIM_CR1_CEN;
    // CCR is preloaded: the value written during a period is output in the next one
    for (size_t i = 1; i < bits; ++i) {
        __HAL_TIM_SET_COMPARE(htim_, channel_, value(i));
        while (!(t->SR & TIM_SR_UIF)) {
        }
        t->SR = ~TIM_SR_UIF;
    }
    __HAL_TIM_SET_COMPARE(htim_, channel_, 0);
    while (!(t->SR & TIM_SR_UIF)) {   // last bit is now active; the line idles low after it
    }
    t->SR = ~TIM_SR_UIF;
}

void Ws2812b::finish_dma() noexcept {
    // The DMA has written the last (low) slot into the CCR preload: the data bits are out
    // or being output. Stop the requests; CCR = 0 keeps the line low.
    __HAL_TIM_DISABLE_DMA(htim_, board_timer::dma_request(channel_));
    __HAL_TIM_SET_COMPARE(htim_, channel_, 0);
    done_ms_.store(HAL_GetTick(), std::memory_order_relaxed);
    busy_.store(false, std::memory_order_release);
}

void Ws2812b::on_pulse_finished(TIM_HandleTypeDef* htim) noexcept {
    for (Ws2812b* led : instances_) {
        if (led && led->htim_ == htim && led->use_dma_ && led->busy()) {
            led->finish_dma();
        }
    }
}
