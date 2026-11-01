#include "board_timer.hpp"

namespace board_timer {

uint32_t clock_hz(const TIM_TypeDef* tim) noexcept {
    const bool apb2 = tim == TIM1 || tim == TIM8 || tim == TIM15 || tim == TIM16 || tim == TIM17 || tim == TIM20;
    if (apb2) {
        const uint32_t pclk = HAL_RCC_GetPCLK2Freq();
        return (RCC->CFGR & RCC_CFGR_PPRE2_2) ? 2u * pclk : pclk;   // timers run at 2x PCLK if APB divided
    }
    const uint32_t pclk = HAL_RCC_GetPCLK1Freq();
    return (RCC->CFGR & RCC_CFGR_PPRE1_2) ? 2u * pclk : pclk;
}

bool is_32bit(const TIM_TypeDef* tim) noexcept {
    return tim == TIM2 || tim == TIM5;
}

bool plan(const TIM_TypeDef* tim, uint32_t hz, Timing& out) noexcept {
    const uint32_t f_tim = clock_hz(tim);
    if (hz == 0 || hz > f_tim / 2u) {
        return false;
    }
    const uint64_t ticks = (static_cast<uint64_t>(f_tim) + hz / 2u) / hz;
    const uint64_t max_period = is_32bit(tim) ? 0x100000000ull : 0x10000ull;
    const uint64_t prescaler = (ticks - 1u) / max_period;
    if (prescaler > 0xFFFFu) {
        return false;
    }
    const uint32_t f_cnt = f_tim / static_cast<uint32_t>(prescaler + 1u);
    const uint64_t period = (static_cast<uint64_t>(f_cnt) + hz / 2u) / hz;
    if (period < 2u) {
        return false;
    }
    out.prescaler = static_cast<uint32_t>(prescaler);
    out.period = static_cast<uint32_t>(period - 1u);
    out.actual_hz = f_cnt / static_cast<uint32_t>(period);
    return true;
}

void apply(TIM_HandleTypeDef* htim, const Timing& timing) noexcept {
    TIM_TypeDef* t = htim->Instance;
    t->PSC = timing.prescaler;
    t->ARR = timing.period;
    htim->Init.Prescaler = timing.prescaler;
    htim->Init.Period = timing.period;
    t->EGR = TIM_EGR_UG;       // load PSC/ARR (and preloaded CCRs) now
    t->SR = ~TIM_SR_UIF;       // UG sets UIF unless URS is set
}

} // namespace board_timer
