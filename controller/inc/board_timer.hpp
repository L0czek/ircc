#ifndef __BOARD_TIMER_HPP__
#define __BOARD_TIMER_HPP__

#include "stm32g4xx_hal.h"
#include <cstdint>

/**
 * @brief Small helpers shared by the timer based drivers (oscilloscope, fan, LEDs).
 *
 * The CubeMX init functions own the timer setup (mode, pins, DMA links); drivers only
 * reprogram PSC/ARR/CCR at runtime, the same way the oscilloscope sets its sample rate.
 */
namespace board_timer {

/** @brief Counter input clock of a timer (APB clock, x2 when the APB prescaler is > 1) */
uint32_t clock_hz(const TIM_TypeDef* tim) noexcept;

/** @brief True for the 32-bit timers (TIM2, TIM5) */
bool is_32bit(const TIM_TypeDef* tim) noexcept;

struct Timing {
    uint32_t prescaler;     // PSC
    uint32_t period;        // ARR
    uint32_t actual_hz;     // resulting update rate
};

/**
 * @brief Pick PSC/ARR for an update rate of `hz`, keeping ARR as large as possible
 *        (best duty resolution). Returns false if `hz` is 0 or above clock/2.
 */
bool plan(const TIM_TypeDef* tim, uint32_t hz, Timing& out) noexcept;

/** @brief Write PSC/ARR and load them now (UG); the counter keeps its run state */
void apply(TIM_HandleTypeDef* htim, const Timing& timing) noexcept;

/** @brief HAL DMA id (TIM_DMA_ID_CCx) for a TIM_CHANNEL_x value */
inline uint16_t dma_id(uint32_t channel) noexcept {
    return static_cast<uint16_t>(TIM_DMA_ID_CC1 + channel / 4u);
}

/** @brief HAL DMA request bit (TIM_DMA_CCx) for a TIM_CHANNEL_x value */
inline uint32_t dma_request(uint32_t channel) noexcept {
    return TIM_DMA_CC1 << (channel / 4u);
}

/** @brief Masks all interrupts for a short critical section */
class IrqLock {
public:
    IrqLock() noexcept : primask_(__get_PRIMASK()) { __disable_irq(); }
    ~IrqLock() { __set_PRIMASK(primask_); }
    IrqLock(const IrqLock&) = delete;
    IrqLock& operator=(const IrqLock&) = delete;
private:
    uint32_t primask_;
};

} // namespace board_timer

#endif
