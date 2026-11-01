#include "oscilloscope.hpp"

#if defined(OSCILLOSCOPE_SUPPORTED)

#include "log.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace {

constexpr uint32_t kRing = Oscilloscope::kRingSamples;
constexpr uint32_t kRegionAlignBytes = 2048;   // PSRAM page in dual-flash bus space; multiple of any burst
constexpr uint32_t kAdcTriggerLatencyHalfCycles = 2 * 8;   // trigger sync + margin, in ADC half cycles
constexpr uint32_t kCompareMarginTicks = 2;

// Masks interrupts for a short critical section shared with the capture ISRs.
class IrqLock {
public:
    IrqLock() : primask_(__get_PRIMASK()) { __disable_irq(); }
    ~IrqLock() { __set_PRIMASK(primask_); }
    IrqLock(const IrqLock&) = delete;
    IrqLock& operator=(const IrqLock&) = delete;
private:
    uint32_t primask_;
};

uint32_t timer_clock_hz(const TIM_TypeDef* tim) {
    const bool apb2 = tim == TIM1 || tim == TIM8 || tim == TIM15 || tim == TIM16 || tim == TIM17 || tim == TIM20;
    if (apb2) {
        const uint32_t pclk = HAL_RCC_GetPCLK2Freq();
        return (RCC->CFGR & RCC_CFGR_PPRE2_2) ? 2u * pclk : pclk;   // timers run at 2x PCLK if APB divided
    }
    const uint32_t pclk = HAL_RCC_GetPCLK1Freq();
    return (RCC->CFGR & RCC_CFGR_PPRE1_2) ? 2u * pclk : pclk;
}

uint32_t adc_clock_hz(const ADC_HandleTypeDef* hadc) {
    switch (hadc->Init.ClockPrescaler) {
        case ADC_CLOCK_SYNC_PCLK_DIV1: return HAL_RCC_GetHCLKFreq();
        case ADC_CLOCK_SYNC_PCLK_DIV2: return HAL_RCC_GetHCLKFreq() / 2u;
        case ADC_CLOCK_SYNC_PCLK_DIV4: return HAL_RCC_GetHCLKFreq() / 4u;
        default: break;
    }
    const bool adc12 = hadc->Instance == ADC1 || hadc->Instance == ADC2;
    const uint32_t kernel = HAL_RCCEx_GetPeriphCLKFreq(adc12 ? RCC_PERIPHCLK_ADC12 : RCC_PERIPHCLK_ADC345);
    static constexpr struct { uint32_t prescaler; uint32_t div; } kAsync[] = {
        {ADC_CLOCK_ASYNC_DIV1, 1},   {ADC_CLOCK_ASYNC_DIV2, 2},     {ADC_CLOCK_ASYNC_DIV4, 4},
        {ADC_CLOCK_ASYNC_DIV6, 6},   {ADC_CLOCK_ASYNC_DIV8, 8},     {ADC_CLOCK_ASYNC_DIV10, 10},
        {ADC_CLOCK_ASYNC_DIV12, 12}, {ADC_CLOCK_ASYNC_DIV16, 16},   {ADC_CLOCK_ASYNC_DIV32, 32},
        {ADC_CLOCK_ASYNC_DIV64, 64}, {ADC_CLOCK_ASYNC_DIV128, 128}, {ADC_CLOCK_ASYNC_DIV256, 256},
    };
    for (const auto& e : kAsync) {
        if (e.prescaler == hadc->Init.ClockPrescaler) {
            return kernel / e.div;
        }
    }
    return 0;
}

// Sampling time in ADC half cycles (0 = unknown)
uint32_t sampling_half_cycles(uint32_t sampling_time) {
    switch (sampling_time) {
        case ADC_SAMPLETIME_2CYCLES_5:   return 5;
        case ADC_SAMPLETIME_6CYCLES_5:   return 13;
        case ADC_SAMPLETIME_12CYCLES_5:  return 25;
        case ADC_SAMPLETIME_24CYCLES_5:  return 49;
        case ADC_SAMPLETIME_47CYCLES_5:  return 95;
        case ADC_SAMPLETIME_92CYCLES_5:  return 185;
        case ADC_SAMPLETIME_247CYCLES_5: return 495;
        case ADC_SAMPLETIME_640CYCLES_5: return 1281;
        default:                         return 0;
    }
}

// Successive-approximation time in ADC half cycles for the configured resolution
uint32_t conversion_half_cycles(const ADC_HandleTypeDef* hadc) {
    switch (hadc->Init.Resolution) {
        case ADC_RESOLUTION_10B: return 21;
        case ADC_RESOLUTION_8B:  return 17;
        case ADC_RESOLUTION_6B:  return 13;
        case ADC_RESOLUTION_12B:
        default:                 return 25;
    }
}

int adc_index(const ADC_HandleTypeDef* hadc) {
    const ADC_TypeDef* i = hadc->Instance;
    if (i == ADC1) return 0;
    if (i == ADC2) return 1;
    if (i == ADC3) return 2;
#if defined(ADC4)
    if (i == ADC4) return 3;
#endif
#if defined(ADC5)
    if (i == ADC5) return 4;
#endif
    return -1;
}

uint32_t injected_rank(uint32_t rank) {
    static constexpr uint32_t kRanks[] = {ADC_INJECTED_RANK_1, ADC_INJECTED_RANK_2, ADC_INJECTED_RANK_3,
                                          ADC_INJECTED_RANK_4};
    return kRanks[rank - 1u];
}

constexpr uint32_t kTim8Requests[scope::kMaxStreams] = {DMA_REQUEST_TIM8_CH1, DMA_REQUEST_TIM8_CH2,
                                                        DMA_REQUEST_TIM8_CH3, DMA_REQUEST_TIM8_CH4};

// Configure compare channel `ch` (0..3) as output compare "frozen" (no output) at `ccr`.
void set_compare_channel(TIM_TypeDef* t, uint32_t ch, uint32_t ccr) {
    volatile uint32_t* ccmr = (ch < 2) ? &t->CCMR1 : &t->CCMR2;
    const uint32_t shift = (ch & 1u) ? 8u : 0u;
    // CCxS = 00 (output), OCxM = 0000 (frozen; bit 16/24 is OCxM[3]), no preload
    const uint32_t mask = (TIM_CCMR1_CC1S | TIM_CCMR1_OC1M | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1FE) << shift;
    *ccmr &= ~mask;
    t->CCER &= ~(TIM_CCER_CC1E << (4u * ch));
    volatile uint32_t* ccr_reg[] = {&t->CCR1, &t->CCR2, &t->CCR3, &t->CCR4};
    *ccr_reg[ch] = ccr;
}

constexpr uint32_t kCcDmaEnable[scope::kMaxStreams] = {TIM_DIER_CC1DE, TIM_DIER_CC2DE, TIM_DIER_CC3DE,
                                                       TIM_DIER_CC4DE};

} // namespace

Oscilloscope* Oscilloscope::active_ = nullptr;
alignas(4) uint16_t Oscilloscope::rings_[scope::kMaxStreams][Oscilloscope::kRingSamples];

Oscilloscope::Oscilloscope(psram::PSRAMDriver& psram, TIM_HandleTypeDef* sample_timer)
    : psram_(psram), timer_(sample_timer), mutex_(nullptr)
{
}

Oscilloscope::~Oscilloscope() {
    (void)stop();
}

// =============================================================================
// Configuration
// =============================================================================

std::expected<void, ControllerError> Oscilloscope::configure(const ScopeConfig& config) noexcept {
    std::lock_guard lock(mutex_);
    if (hardware_running_) {
        return std::unexpected(ControllerError::HAL_BUSY);
    }
    configured_ = false;

    if (!timer_ || timer_->Instance != TIM8) {
        // Injected trigger (T8_TRGO) and DMA requests (TIM8_CHx) are tied to TIM8.
        os::error("Oscilloscope: sample timer must be TIM8\n");
        return std::unexpected(ControllerError::OscilloscopeNotConfigured);
    }
    if (config.num_channels == 0 || config.num_channels > scope::kMaxStreams || config.sample_rate_hz == 0) {
        os::error("Oscilloscope: need 1..%u channels and a sample rate\n", static_cast<unsigned>(scope::kMaxStreams));
        return std::unexpected(ControllerError::OscilloscopeInvalidChannel);
    }
    if (config.trigger.analog_watchdog && config.trigger.awd_stream >= config.num_channels) {
        return std::unexpected(ControllerError::OscilloscopeInvalidChannel);
    }
    if (!psram_.psram_is_initialized()) {
        os::error("Oscilloscope: PSRAM not initialised\n");
        return std::unexpected(ControllerError::PSRAM_NotInitialized);
    }

    // Streams: one injected channel each, ranks assigned per ADC in channel order.
    std::array<uint32_t, 5> per_adc{};
    for (uint32_t s = 0; s < config.num_channels; ++s) {
        const ScopeChannel& c = config.channels[s];
        const int a = c.adc ? adc_index(c.adc) : -1;
        if (a < 0 || sampling_half_cycles(c.sampling_time) == 0) {
            os::error("Oscilloscope: channel %u: invalid ADC or sampling time\n", static_cast<unsigned>(s));
            return std::unexpected(ControllerError::OscilloscopeInvalidChannel);
        }
        if (++per_adc[a] > 4) {
            os::error("Oscilloscope: more than 4 injected channels on ADC%d\n", a + 1);
            return std::unexpected(ControllerError::OscilloscopeInvalidChannel);
        }
        DMA_HandleTypeDef* dma = timer_->hdma[TIM_DMA_ID_CC1 + s];
        if (!dma || !dma->Instance || dma->Init.Request != kTim8Requests[s] ||
            dma->Init.Direction != DMA_PERIPH_TO_MEMORY || dma->Init.Mode != DMA_CIRCULAR ||
            dma->Init.MemDataAlignment != DMA_MDATAALIGN_HALFWORD || dma->Init.MemInc != DMA_MINC_ENABLE ||
            dma->Init.PeriphInc != DMA_PINC_DISABLE) {
            os::error("Oscilloscope: channel %u needs DMA request TIM8_CH%u (periph->mem, circular, "
                      "memory halfword) in CubeMX\n", static_cast<unsigned>(s), static_cast<unsigned>(s + 1));
            return std::unexpected(ControllerError::OscilloscopeNotConfigured);
        }
        Stream& st = streams_[s];
        st = Stream{};
        st.adc = c.adc;
        st.channel = c.channel;
        st.sampling_time = c.sampling_time;
        st.rank = per_adc[a];
        st.dma = dma;
    }
    num_streams_ = config.num_channels;
    config_ = config;

    if (auto r = plan_timing(); !r) {
        return r;
    }

    // PSRAM layout: one ring region per stream, page aligned.
    const size_t burst_bytes = psram_.psram_dma_burst_bytes();
    if (burst_bytes == 0 || (kRing % (burst_bytes / 2u * 2u)) != 0) {
        os::error("Oscilloscope: PSRAM clock too slow for DMA bursts\n");
        return std::unexpected(ControllerError::PSRAM_ClockTooSlow);
    }
    const uint32_t region_bytes =
        static_cast<uint32_t>(psram_.psram_get_size() / num_streams_) / kRegionAlignBytes * kRegionAlignBytes;
    region_samples_ = region_bytes / 2u;
    for (uint32_t s = 0; s < num_streams_; ++s) {
        streams_[s].psram_base = s * region_bytes;
    }

    scope::CoreConfig core{};
    core.num_streams = num_streams_;
    core.ring_samples = kRing;
    core.burst_samples = static_cast<uint32_t>(burst_bytes / 2u);
    core.region_samples = region_samples_;
    core.pre_samples = config.pre_trigger_samples;
    core.post_samples = config.post_trigger_samples;
    core.require_full_pretrigger = config.trigger.require_full_pretrigger;
    if (!core_.configure(core)) {
        os::error("Oscilloscope: pre (%lu) + post (%lu) + %lu exceeds %lu samples per channel\n",
                  static_cast<unsigned long>(config.pre_trigger_samples),
                  static_cast<unsigned long>(config.post_trigger_samples), static_cast<unsigned long>(kRing),
                  static_cast<unsigned long>(region_samples_));
        return std::unexpected(ControllerError::OscilloscopeBufferOverflow);
    }

    configured_ = true;
    return {};
}

std::expected<void, ControllerError> Oscilloscope::plan_timing() noexcept {
    const uint32_t f_tim = timer_clock_hz(timer_->Instance);
    const uint64_t ticks = (static_cast<uint64_t>(f_tim) + config_.sample_rate_hz / 2u) / config_.sample_rate_hz;
    if (ticks < 4) {
        return std::unexpected(ControllerError::OscilloscopeNotConfigured);
    }
    prescaler_ = static_cast<uint32_t>((ticks - 1u) / 65536u);
    const uint32_t f_cnt = f_tim / (prescaler_ + 1u);
    period_ = static_cast<uint32_t>((static_cast<uint64_t>(f_cnt) + config_.sample_rate_hz / 2u) /
                                    config_.sample_rate_hz) - 1u;
    actual_rate_hz_ = f_cnt / (period_ + 1u);

    // Each stream's JDR is copied once its injected rank has been converted.
    for (uint32_t s = 0; s < num_streams_; ++s) {
        Stream& st = streams_[s];
        const uint32_t f_adc = adc_clock_hz(st.adc);
        if (f_adc == 0) {
            return std::unexpected(ControllerError::OscilloscopeNotConfigured);
        }
        uint64_t half_cycles = kAdcTriggerLatencyHalfCycles;
        for (uint32_t j = 0; j < num_streams_; ++j) {
            if (streams_[j].adc == st.adc && streams_[j].rank <= st.rank) {
                half_cycles += sampling_half_cycles(streams_[j].sampling_time) + conversion_half_cycles(st.adc);
            }
        }
        const uint64_t ccr = (half_cycles * f_cnt + 2ull * f_adc - 1u) / (2ull * f_adc) + kCompareMarginTicks;
        if (ccr + 1u >= period_) {
            os::error("Oscilloscope: %lu Hz too fast for channel %lu sampling time (needs %llu of %lu timer ticks)\n",
                      static_cast<unsigned long>(config_.sample_rate_hz), static_cast<unsigned long>(s),
                      static_cast<unsigned long long>(ccr + 1u), static_cast<unsigned long>(period_));
            return std::unexpected(ControllerError::OscilloscopeNotConfigured);
        }
        st.ccr = static_cast<uint32_t>(ccr);
    }
    return {};
}

// =============================================================================
// Run control
// =============================================================================

std::expected<void, ControllerError> Oscilloscope::setup_adcs() noexcept {
    for (uint32_t s = 0; s < num_streams_; ++s) {
        ADC_HandleTypeDef* adc = streams_[s].adc;
        if (streams_[s].rank == 1) {
            // First stream of this ADC: stop it and (once) calibrate while disabled.
            if (HAL_ADC_GetState(adc) & HAL_ADC_STATE_INJ_BUSY) {
                (void)HAL_ADCEx_InjectedStop(adc);
            }
            const int a = adc_index(adc);
            if (!calibrated_[a] && (adc->Instance->CR & ADC_CR_ADEN) == 0) {
                if (HAL_ADCEx_Calibration_Start(adc, ADC_SINGLE_ENDED) == HAL_OK) {
                    calibrated_[a] = true;
                }
            }
        }
        uint32_t on_adc = 0;
        for (uint32_t j = 0; j < num_streams_; ++j) {
            on_adc += (streams_[j].adc == adc) ? 1u : 0u;
        }

        ADC_InjectionConfTypeDef inj = {};
        inj.InjectedChannel = streams_[s].channel;
        inj.InjectedRank = injected_rank(streams_[s].rank);
        inj.InjectedSamplingTime = streams_[s].sampling_time;
        inj.InjectedSingleDiff = ADC_SINGLE_ENDED;
        inj.InjectedOffsetNumber = ADC_OFFSET_NONE;
        inj.InjectedOffset = 0;
        inj.InjectedOffsetSign = ADC_OFFSET_SIGN_NEGATIVE;
        inj.InjectedOffsetSaturation = DISABLE;
        inj.InjectedNbrOfConversion = on_adc;
        inj.InjectedDiscontinuousConvMode = DISABLE;
        inj.AutoInjectedConv = DISABLE;
        inj.QueueInjectedContext = DISABLE;
        inj.ExternalTrigInjecConv = ADC_EXTERNALTRIGINJEC_T8_TRGO;
        inj.ExternalTrigInjecConvEdge = ADC_EXTERNALTRIGINJECCONV_EDGE_RISING;
        inj.InjecOversamplingMode = DISABLE;
        if (HAL_StatusTypeDef st = HAL_ADCEx_InjectedConfigChannel(adc, &inj); st != HAL_OK) {
            os::error("Oscilloscope: injected config of channel %u failed (%d)\n", static_cast<unsigned>(s),
                      static_cast<int>(st));
            return std::unexpected(from_hal_status(st));
        }

        if (config_.trigger.analog_watchdog && config_.trigger.awd_stream == s) {
            ADC_AnalogWDGConfTypeDef awd = {};
            awd.WatchdogNumber = ADC_ANALOGWATCHDOG_1;
            awd.WatchdogMode = ADC_ANALOGWATCHDOG_SINGLE_INJEC;
            awd.Channel = streams_[s].channel;
            awd.ITMode = DISABLE;   // enabled once the pre-trigger holdoff has passed
            awd.HighThreshold = config_.trigger.awd_high;
            awd.LowThreshold = config_.trigger.awd_low;
            awd.FilteringConfig = ADC_AWD_FILTERING_NONE;
            if (HAL_StatusTypeDef st = HAL_ADC_AnalogWDGConfig(adc, &awd); st != HAL_OK) {
                return std::unexpected(from_hal_status(st));
            }
            __HAL_ADC_CLEAR_FLAG(adc, ADC_FLAG_AWD1);
        }
    }
    return {};
}

std::expected<void, ControllerError> Oscilloscope::start_stream_dmas() noexcept {
    for (uint32_t s = 0; s < num_streams_; ++s) {
        Stream& st = streams_[s];
        if (st.dma->State != HAL_DMA_STATE_READY) {
            (void)HAL_DMA_Abort(st.dma);
        }
        st.laps = 0;
        st.dma->XferHalfCpltCallback = &Oscilloscope::dma_half_isr;
        st.dma->XferCpltCallback = &Oscilloscope::dma_full_isr;
        st.dma->XferErrorCallback = &Oscilloscope::dma_error_isr;
        st.dma->XferAbortCallback = nullptr;
        // JDR1..JDR4 are consecutive registers; the stream reads the JDR of its rank.
        const uint32_t src = reinterpret_cast<uint32_t>(&st.adc->Instance->JDR1 + (st.rank - 1u));
        const uint32_t dst = reinterpret_cast<uint32_t>(&rings_[s][0]);
        if (HAL_StatusTypeDef r = HAL_DMA_Start_IT(st.dma, src, dst, kRing); r != HAL_OK) {
            return std::unexpected(ControllerError::OscilloscopeDMAError);
        }
    }
    return {};
}

void Oscilloscope::start_timer() noexcept {
    TIM_TypeDef* t = timer_->Instance;
    t->CR1 &= ~TIM_CR1_CEN;
    t->DIER &= ~(TIM_DIER_CC1DE | TIM_DIER_CC2DE | TIM_DIER_CC3DE | TIM_DIER_CC4DE | TIM_DIER_UDE);
    t->PSC = prescaler_;
    t->ARR = period_;
    for (uint32_t s = 0; s < num_streams_; ++s) {
        set_compare_channel(t, s, streams_[s].ccr);
    }
    t->CR2 = (t->CR2 & ~TIM_CR2_MMS) | TIM_TRGO_UPDATE;   // TRGO on update -> injected trigger
    t->CNT = 0;
    // UG loads PSC/ARR and raises TRGO: sample 0 is converted now and copied at CCRn,
    // then one sample per period.
    t->EGR = TIM_EGR_UG;
    t->SR = 0;
    uint32_t dier = 0;
    for (uint32_t s = 0; s < num_streams_; ++s) {
        dier |= kCcDmaEnable[s];
    }
    t->DIER |= dier;
    t->CR1 |= TIM_CR1_CEN;
}

void Oscilloscope::stop_timer() noexcept {
    TIM_TypeDef* t = timer_->Instance;
    t->CR1 &= ~TIM_CR1_CEN;
    t->DIER &= ~(TIM_DIER_CC1DE | TIM_DIER_CC2DE | TIM_DIER_CC3DE | TIM_DIER_CC4DE);
    __DSB();
}

std::expected<void, ControllerError> Oscilloscope::arm() noexcept {
    std::lock_guard lock(mutex_);
    if (!configured_) {
        return std::unexpected(ControllerError::OscilloscopeNotConfigured);
    }
    if (hardware_running_) {
        return std::unexpected(ControllerError::HAL_BUSY);
    }
    if (active_ && active_ != this) {
        return std::unexpected(ControllerError::HAL_BUSY);
    }

    if (auto r = psram_.psram_dma_begin(&Oscilloscope::burst_done_isr, this); !r) {
        os::error("Oscilloscope: PSRAM DMA unavailable: %s\n", r.error().message().c_str());
        return std::unexpected(from_psram_error(r.error()));
    }
    last_psram_error_ = ControllerError::HAL_OK;
    active_ = this;

    auto fail_arm = [&](ControllerError e) -> std::expected<void, ControllerError> {
        release_hardware();
        return std::unexpected(e);
    };

    stop_timer();
    if (auto r = setup_adcs(); !r) {
        return fail_arm(r.error());
    }
    if (auto r = start_stream_dmas(); !r) {
        return fail_arm(r.error());
    }
    {
        IrqLock irq;
        core_.arm();
        hardware_running_ = true;
    }
    for (uint32_t s = 0; s < num_streams_; ++s) {
        if (streams_[s].rank == 1) {
            if (HAL_StatusTypeDef st = HAL_ADCEx_InjectedStart(streams_[s].adc); st != HAL_OK) {
                return fail_arm(from_hal_status(st));
            }
        }
    }
    if (config_.trigger.analog_watchdog &&
        (!config_.trigger.require_full_pretrigger || config_.pre_trigger_samples == 0)) {
        __HAL_ADC_ENABLE_IT(streams_[config_.trigger.awd_stream].adc, ADC_IT_AWD1);
    }
    start_timer();
    return {};
}

std::expected<void, ControllerError> Oscilloscope::force_trigger() noexcept {
    if (!config_.trigger.software) {
        return std::unexpected(ControllerError::OscilloscopeNotConfigured);
    }
    if (!hardware_running_ || core_.state() != scope::State::PreTrigger) {
        return std::unexpected(ControllerError::OscilloscopeNotConfigured);
    }
    trigger_from_isr(ScopeTriggerSource::Software);
    if (core_.state() == scope::State::PreTrigger) {
        return std::unexpected(ControllerError::OscilloscopeBufferEmpty);   // holdoff: pre-trigger not filled yet
    }
    return {};
}

std::expected<void, ControllerError> Oscilloscope::stop() noexcept {
    std::lock_guard lock(mutex_);
    if (!hardware_running_) {
        return {};
    }
    {
        IrqLock irq;
        stop_timer();
        core_.abort();
    }
    release_hardware();
    return {};
}

void Oscilloscope::release_hardware() noexcept {
    {
        IrqLock irq;
        stop_timer();
        hardware_running_ = false;
        for (uint32_t s = 0; s < num_streams_; ++s) {
            Stream& st = streams_[s];
            if (st.dma) {
                (void)HAL_DMA_Abort(st.dma);
                st.dma->XferHalfCpltCallback = nullptr;
                st.dma->XferCpltCallback = nullptr;
                st.dma->XferErrorCallback = nullptr;
            }
        }
        (void)psram_.psram_dma_end();
        if (active_ == this) {
            active_ = nullptr;
        }
    }
    // ADC stop polls with HAL_GetTick timeouts: outside the critical section.
    for (uint32_t s = 0; s < num_streams_; ++s) {
        ADC_HandleTypeDef* adc = streams_[s].adc;
        if (config_.trigger.analog_watchdog && config_.trigger.awd_stream == s) {
            __HAL_ADC_DISABLE_IT(adc, ADC_IT_AWD1);
        }
        if (streams_[s].rank == 1) {
            (void)HAL_ADCEx_InjectedStop(adc);
        }
    }
}

scope::State Oscilloscope::state() const noexcept {
    return core_.state();
}

ControllerError Oscilloscope::fault_error() const noexcept {
    switch (core_.fault()) {
        case scope::Fault::Overrun:    return ControllerError::OscilloscopeOverrun;
        case scope::Fault::WriteError: return last_psram_error_ != ControllerError::HAL_OK
                                              ? last_psram_error_ : ControllerError::OscilloscopeDMAError;
        case scope::Fault::DmaError:   return ControllerError::OscilloscopeDMAError;
        case scope::Fault::Aborted:    return ControllerError::OscilloscopeAborted;
        case scope::Fault::None:
        default:                       return ControllerError::UnknownError;
    }
}

std::expected<ScopeCaptureInfo, ControllerError> Oscilloscope::make_info() const noexcept {
    if (core_.state() == scope::State::Error) {
        return std::unexpected(fault_error());
    }
    if (core_.state() != scope::State::Done) {
        return std::unexpected(ControllerError::OscilloscopeBufferEmpty);
    }
    const scope::Window w = core_.window();
    ScopeCaptureInfo info{};
    info.trigger_source = static_cast<ScopeTriggerSource>(core_.trigger_source());
    info.num_channels = num_streams_;
    info.sample_rate_hz = actual_rate_hz_;
    info.trigger_index = w.trigger;
    info.first_index = w.first;
    info.end_index = w.end;
    return info;
}

std::expected<ScopeCaptureInfo, ControllerError> Oscilloscope::wait_done(uint32_t timeout_ms) noexcept {
    const uint32_t start = os::timestamp_ms();
    for (;;) {
        const scope::State st = core_.state();
        if (st == scope::State::Done || st == scope::State::Error) {
            std::lock_guard lock(mutex_);
            if (hardware_running_) {
                release_hardware();
            }
            auto info = make_info();
            if (!info) {
                os::error("Oscilloscope: capture failed: %s\n", to_string(info.error()));
            }
            return info;
        }
        if (st == scope::State::Idle) {
            return std::unexpected(ControllerError::OscilloscopeNotConfigured);
        }
        if (os::timestamp_ms() - start >= timeout_ms) {
            return std::unexpected(ControllerError::HAL_TIMEOUT);
        }
        os::delay(1);
    }
}

std::expected<ScopeCaptureInfo, ControllerError> Oscilloscope::info() const noexcept {
    std::lock_guard lock(mutex_);
    return make_info();
}

std::expected<void, ControllerError> Oscilloscope::read(uint32_t channel, int64_t offset, uint16_t* out,
                                                        uint32_t count) noexcept {
    std::lock_guard lock(mutex_);
    if (hardware_running_ || core_.state() != scope::State::Done) {
        return std::unexpected(ControllerError::OscilloscopeBufferEmpty);
    }
    if (channel >= num_streams_ || !out) {
        return std::unexpected(ControllerError::OscilloscopeInvalidChannel);
    }
    const scope::Window w = core_.window();
    const int64_t first = static_cast<int64_t>(w.trigger) + offset;
    if (first < static_cast<int64_t>(w.first) || first + count > static_cast<int64_t>(w.end)) {
        return std::unexpected(ControllerError::OscilloscopeOutOfRange);
    }
    uint64_t index = static_cast<uint64_t>(first);
    while (count > 0) {
        const uint32_t in_region = static_cast<uint32_t>(index % region_samples_);
        const uint32_t n = std::min(count, region_samples_ - in_region);
        const uint32_t address = streams_[channel].psram_base + in_region * 2u;
        if (auto r = psram_.psram_read(address, reinterpret_cast<uint8_t*>(out), n * 2u); !r) {
            return std::unexpected(from_psram_error(r.error()));
        }
        out += n;
        index += n;
        count -= n;
    }
    return {};
}

// =============================================================================
// Interrupt side
// =============================================================================

int Oscilloscope::stream_of(const DMA_HandleTypeDef* hdma) const noexcept {
    for (uint32_t s = 0; s < num_streams_; ++s) {
        if (streams_[s].dma == hdma) {
            return static_cast<int>(s);
        }
    }
    return -1;
}

// Absolute number of samples the stream's DMA has written.
uint64_t Oscilloscope::position(uint32_t s) const noexcept {
    const Stream& st = streams_[s];
    const uint32_t tc_flag = DMA_FLAG_TC1 << (st.dma->ChannelIndex & 0x1Fu);
    uint32_t laps;
    uint32_t remaining;
    bool tc_pending;
    do {   // consistent snapshot of laps / TC flag / CNDTR
        laps = st.laps;
        tc_pending = (st.dma->DmaBaseAddress->ISR & tc_flag) != 0u;
        remaining = st.dma->Instance->CNDTR;
    } while (laps != st.laps || tc_pending != ((st.dma->DmaBaseAddress->ISR & tc_flag) != 0u));
    uint64_t pos = static_cast<uint64_t>(laps) * kRing + (kRing - remaining);
    if (tc_pending) {
        pos += kRing;   // wrapped, transfer-complete not yet handled
    }
    if (pos < core_.produced(s)) {
        pos += kRing;   // preempted a handler between flag clear and lap update
    }
    return pos;
}

void Oscilloscope::dma_half_isr(DMA_HandleTypeDef* hdma) {
    if (Oscilloscope* self = active_) {
        if (const int s = self->stream_of(hdma); s >= 0) {
            self->on_ring_event_isr(static_cast<uint32_t>(s), false);
        }
    }
}

void Oscilloscope::dma_full_isr(DMA_HandleTypeDef* hdma) {
    if (Oscilloscope* self = active_) {
        if (const int s = self->stream_of(hdma); s >= 0) {
            self->on_ring_event_isr(static_cast<uint32_t>(s), true);
        }
    }
}

void Oscilloscope::dma_error_isr(DMA_HandleTypeDef* hdma) {
    if (Oscilloscope* self = active_) {
        if (self->stream_of(hdma) >= 0) {
            self->fail_isr(scope::Fault::DmaError);
        }
    }
}

void Oscilloscope::on_ring_event_isr(uint32_t s, bool full) noexcept {
    if (!hardware_running_) {
        return;
    }
    const scope::State state = core_.state();
    if (state != scope::State::PreTrigger && state != scope::State::PostTrigger) {
        return;   // production stopped: late events carry no new data
    }
    Stream& st = streams_[s];
    if (full) {
        st.laps = st.laps + 1u;
    }
    const uint64_t committed = static_cast<uint64_t>(st.laps) * kRing + (full ? 0u : kRing / 2u);
    // If the DMA is already past the next half, an event was handled too late (or
    // lost): data may have been overwritten unnoticed.
    if (position(s) >= committed + kRing / 2u) {
        fail_isr(scope::Fault::Overrun);
        return;
    }
    core_.on_produced(s, committed);
    if (core_.state() == scope::State::Error) {
        fail_isr(core_.fault());
        return;
    }

    // Analog watchdog: armed only after the pre-trigger holdoff (avoids an interrupt
    // per conversion while the signal is outside the window during holdoff).
    if (config_.trigger.analog_watchdog && s == config_.trigger.awd_stream &&
        core_.state() == scope::State::PreTrigger && committed >= config_.pre_trigger_samples) {
        ADC_HandleTypeDef* adc = streams_[s].adc;
        if ((adc->Instance->IER & ADC_IT_AWD1) == 0u) {
            __HAL_ADC_CLEAR_FLAG(adc, ADC_FLAG_AWD1);
            __HAL_ADC_ENABLE_IT(adc, ADC_IT_AWD1);
        }
    }

    if (core_.should_stop_production()) {
        stop_production_isr();
    }
    kick_writer_isr();
}

void Oscilloscope::stop_production_isr() noexcept {
    stop_timer();
    std::array<uint64_t, scope::kMaxStreams> final_count{};
    for (uint32_t s = 0; s < num_streams_; ++s) {
        final_count[s] = position(s);
    }
    core_.production_stopped(final_count);
    if (core_.state() == scope::State::Error) {
        fail_isr(core_.fault());
    }
}

void Oscilloscope::kick_writer_isr() noexcept {
    if (psram_.psram_dma_busy() || core_.burst_in_flight()) {
        return;
    }
    const auto burst = core_.next_burst();
    if (!burst) {
        return;
    }
    const uint32_t address = streams_[burst->stream].psram_base + burst->region_offset * 2u;
    auto r = psram_.psram_dma_write(address, &rings_[burst->stream][burst->ring_offset], burst->samples * 2u);
    if (!r) {
        last_psram_error_ = from_psram_error(r.error());
        core_.burst_done(false);
        fail_isr(scope::Fault::WriteError);
    }
}

void Oscilloscope::burst_done_isr(void* context, std::error_code result) {
    auto* self = static_cast<Oscilloscope*>(context);
    if (!self->hardware_running_) {
        return;
    }
    if (result) {
        self->last_psram_error_ = from_psram_error(result);
        self->core_.burst_done(false);
        self->fail_isr(scope::Fault::WriteError);
        return;
    }
    self->core_.burst_done(true);
    self->kick_writer_isr();
}

void Oscilloscope::fail_isr(scope::Fault fault) noexcept {
    stop_timer();
    core_.fail(fault);
}

void Oscilloscope::trigger_from_isr(ScopeTriggerSource source) noexcept {
    if (active_ != this || !hardware_running_) {
        return;
    }
    switch (source) {
        case ScopeTriggerSource::Software:       if (!config_.trigger.software) return; break;
        case ScopeTriggerSource::AnalogWatchdog: if (!config_.trigger.analog_watchdog) return; break;
#if defined(HAL_COMP_MODULE_ENABLED)
        case ScopeTriggerSource::Comparator:     if (!config_.trigger.comparator) return; break;
#else
        case ScopeTriggerSource::Comparator:     return;
#endif
        case ScopeTriggerSource::External:       if (!config_.trigger.external) return; break;
        default:                                 return;
    }
    IrqLock irq;
    if (core_.state() != scope::State::PreTrigger) {
        return;
    }
    // Trigger instant = number of samples stream 0 has delivered (± interrupt latency).
    (void)core_.trigger(position(0), static_cast<uint8_t>(source));
}

void Oscilloscope::on_analog_watchdog_isr(ADC_HandleTypeDef* hadc) noexcept {
    if (!config_.trigger.analog_watchdog || streams_[config_.trigger.awd_stream].adc != hadc) {
        return;
    }
    // One interrupt per arming; re-armed from the ring events if still waiting.
    __HAL_ADC_DISABLE_IT(hadc, ADC_IT_AWD1);
    trigger_from_isr(ScopeTriggerSource::AnalogWatchdog);
}

#if defined(HAL_COMP_MODULE_ENABLED)
void Oscilloscope::on_comparator_isr(COMP_HandleTypeDef* hcomp) noexcept {
    if (config_.trigger.comparator == hcomp) {
        trigger_from_isr(ScopeTriggerSource::Comparator);
    }
}
#endif

// =============================================================================
// HAL weak callback overrides
// =============================================================================

extern "C" void HAL_ADC_LevelOutOfWindowCallback(ADC_HandleTypeDef* hadc) {
    if (Oscilloscope* scope = Oscilloscope::active()) {
        scope->on_analog_watchdog_isr(hadc);
    }
}

#if defined(HAL_COMP_MODULE_ENABLED)
extern "C" void HAL_COMP_TriggerCallback(COMP_HandleTypeDef* hcomp) {
    if (Oscilloscope* scope = Oscilloscope::active()) {
        scope->on_comparator_isr(hcomp);
    }
}
#endif

#endif // OSCILLOSCOPE_SUPPORTED
