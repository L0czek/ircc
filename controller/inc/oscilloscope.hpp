#ifndef __OSCILLOSCOPE_HPP__
#define __OSCILLOSCOPE_HPP__

#include "board.h"
#include "error.hpp"
#include "mutex.hpp"
#include "scope_capture_core.hpp"
#include <array>
#include <cstdint>
#include <expected>

#if defined(HAL_ADC_MODULE_ENABLED) && defined(HAL_QSPI_MODULE_ENABLED) && \
    defined(HAL_DMA_MODULE_ENABLED) && defined(HAL_TIM_MODULE_ENABLED)
#define OSCILLOSCOPE_SUPPORTED 1

#include "aps6404l.hpp"

/**
 * @brief Long-trace oscilloscope: injected ADC conversions streamed into PSRAM
 *
 * Data path (no CPU per sample):
 *
 *   sample timer (TIM8) TRGO ──► injected conversion on every used ADC (JEXTSEL = T8_TRGO)
 *   TIM8 CCn (n = stream + 1), placed after the conversion ends
 *        ──► DMA request TIM8_CHn ──► DMA copies ADC JDRx into the stream's SRAM ring (circular)
 *   SRAM ring half/full events ──► PSRAM writer: QUADSPI indirect-write bursts fed by the
 *        QUADSPI DMA (psram::PSRAMDriver::psram_dma_write), chained from its TC interrupt
 *
 * Injected conversions have no ADC DMA request (RM0440 21.4.26: DMA serves regular
 * conversions only), so the copy is paced by the sample timer's compare channels. Each
 * stream is one injected channel and owns one TIM8 compare channel: up to 4 streams.
 * The regular group of the ADCs stays available to other code; injected conversions
 * preempt it.
 *
 * Capture: a pre-trigger ring per stream in PSRAM. Recording runs until a trigger
 * (software, ADC analog watchdog on a stream, comparator, or external via
 * trigger_from_isr()) and then for post_trigger_samples more; see scope::CaptureCore.
 *
 * CubeMX requirements (validated at configure()/arm(), errors are logged):
 *  - TIM8 (sample timer): TRGO = update. Channels 1..N: output compare, no output.
 *    DMA requests TIM8_CH1..N: peripheral-to-memory, circular, memory halfword,
 *    peripheral word or halfword, priority high, interrupt enabled.
 *  - ADCs used: injected trigger TIM8_TRGO (the oscilloscope sets the sequence itself).
 *  - QUADSPI: DMA request (memory-to-peripheral, normal), interrupt of that DMA channel
 *    enabled, FifoThreshold >= 4, timing valid for the PSRAM (see PSRAMDriver).
 *  - All the DMA channels above at the same NVIC priority (their handlers must not
 *    preempt each other). Trigger interrupts (ADC, COMP EXTI) at the same or lower
 *    priority. Analog watchdog trigger needs the ADC global interrupt; comparator
 *    trigger needs the COMP EXTI interrupt (rising edge) and the comparator started.
 *
 * The oscilloscope overrides the weak HAL callbacks HAL_ADC_LevelOutOfWindowCallback
 * and HAL_COMP_TriggerCallback.
 */

enum class ScopeTriggerSource : uint8_t {
    Software = 0,
    AnalogWatchdog = 1,
    Comparator = 2,
    External = 3,
};

struct ScopeChannel {
    ADC_HandleTypeDef* adc = nullptr;
    uint32_t channel = 0;                              // ADC_CHANNEL_x
    uint32_t sampling_time = ADC_SAMPLETIME_6CYCLES_5; // ADC_SAMPLETIME_x
};

struct ScopeTriggerConfig {
    bool software = true;

    // Analog watchdog 1 on one stream: trigger when the sample leaves [low, high]
    bool analog_watchdog = false;
    uint32_t awd_stream = 0;
    uint16_t awd_low = 0;
    uint16_t awd_high = 4095;

#if defined(HAL_COMP_MODULE_ENABLED)
    COMP_HandleTypeDef* comparator = nullptr;   // trigger on its EXTI event
#endif

    bool external = false;                      // accept trigger_from_isr(External)
    bool require_full_pretrigger = true;        // ignore triggers until pre samples recorded
};

struct ScopeConfig {
    std::array<ScopeChannel, scope::kMaxStreams> channels{};
    uint32_t num_channels = 0;
    uint32_t sample_rate_hz = 0;
    uint32_t pre_trigger_samples = 0;
    uint32_t post_trigger_samples = 0;
    ScopeTriggerConfig trigger{};
};

struct ScopeCaptureInfo {
    ScopeTriggerSource trigger_source;
    uint32_t num_channels;
    uint32_t sample_rate_hz;   // actual rate (timer quantised)
    uint64_t trigger_index;    // absolute sample index of the trigger
    uint64_t first_index;      // oldest sample available
    uint64_t end_index;        // one past the last sample (trigger + post)
};

class Oscilloscope {
public:
    static constexpr uint32_t kRingSamples = 4096;   // SRAM ring per stream (8 KB)

    Oscilloscope(psram::PSRAMDriver& psram, TIM_HandleTypeDef* sample_timer);
    ~Oscilloscope();
    Oscilloscope(const Oscilloscope&) = delete;
    Oscilloscope& operator=(const Oscilloscope&) = delete;

    /** @brief Validate and store a configuration (not running) */
    std::expected<void, ControllerError> configure(const ScopeConfig& config) noexcept;

    /** @brief Start recording into the pre-trigger ring */
    std::expected<void, ControllerError> arm() noexcept;

    /** @brief Software trigger (if enabled in the configuration) */
    std::expected<void, ControllerError> force_trigger() noexcept;

    /** @brief Abort a running capture */
    std::expected<void, ControllerError> stop() noexcept;

    /**
     * @brief Wait until the capture is complete (or failed) and release the hardware.
     * @return capture info, HAL_TIMEOUT if still running after timeout_ms
     */
    std::expected<ScopeCaptureInfo, ControllerError> wait_done(uint32_t timeout_ms) noexcept;

    scope::State state() const noexcept;

    /** @brief Info of the last completed capture */
    std::expected<ScopeCaptureInfo, ControllerError> info() const noexcept;

    /**
     * @brief Copy samples of a completed capture
     * @param channel  stream index (order of ScopeConfig::channels)
     * @param offset   first sample relative to the trigger (negative = pre-trigger)
     */
    std::expected<void, ControllerError> read(uint32_t channel, int64_t offset, uint16_t* out,
                                              uint32_t count) noexcept;

    /** @brief Trigger from an interrupt (HRTIM fault, user code, ...); ISR safe */
    void trigger_from_isr(ScopeTriggerSource source) noexcept;

    // ---- interrupt glue (called from HAL callbacks) ----
    static Oscilloscope* active() noexcept { return active_; }
    void on_analog_watchdog_isr(ADC_HandleTypeDef* hadc) noexcept;
#if defined(HAL_COMP_MODULE_ENABLED)
    void on_comparator_isr(COMP_HandleTypeDef* hcomp) noexcept;
#endif

private:
    struct Stream {
        ADC_HandleTypeDef* adc = nullptr;
        uint32_t channel = 0;
        uint32_t sampling_time = 0;
        uint32_t rank = 0;                    // 1..4 within its ADC's injected sequence
        uint32_t ccr = 0;                     // TIM8 compare value for the JDR copy
        DMA_HandleTypeDef* dma = nullptr;     // TIM8_CHn DMA
        volatile uint32_t laps = 0;           // completed passes over the SRAM ring
        uint32_t psram_base = 0;              // byte address of the stream's PSRAM region
    };

    psram::PSRAMDriver& psram_;
    TIM_HandleTypeDef* timer_;
    mutable os::mutex mutex_;

    ScopeConfig config_{};
    bool configured_ = false;
    bool hardware_running_ = false;
    std::array<Stream, scope::kMaxStreams> streams_{};
    uint32_t num_streams_ = 0;
    uint32_t prescaler_ = 0;
    uint32_t period_ = 0;
    uint32_t actual_rate_hz_ = 0;
    uint32_t region_samples_ = 0;
    std::array<bool, 5> calibrated_{};        // ADC1..ADC5

    scope::CaptureCore core_;
    volatile bool psram_error_ = false;
    ControllerError last_psram_error_ = ControllerError::HAL_OK;

    static Oscilloscope* active_;
    static uint16_t rings_[scope::kMaxStreams][kRingSamples];

    // configuration helpers
    std::expected<void, ControllerError> plan_timing() noexcept;
    std::expected<void, ControllerError> setup_adcs() noexcept;
    std::expected<void, ControllerError> start_stream_dmas() noexcept;
    void start_timer() noexcept;
    void stop_timer() noexcept;
    void release_hardware() noexcept;
    std::expected<ScopeCaptureInfo, ControllerError> make_info() const noexcept;
    ControllerError fault_error() const noexcept;

    // interrupt-side helpers (DMA handlers share one priority, no locking between them)
    uint64_t position(uint32_t stream) const noexcept;
    void on_ring_event_isr(uint32_t stream, bool full) noexcept;
    void stop_production_isr() noexcept;
    void kick_writer_isr() noexcept;
    void fail_isr(scope::Fault fault) noexcept;
    static void dma_half_isr(DMA_HandleTypeDef* hdma);
    static void dma_full_isr(DMA_HandleTypeDef* hdma);
    static void dma_error_isr(DMA_HandleTypeDef* hdma);
    static void burst_done_isr(void* context, std::error_code result);
    int stream_of(const DMA_HandleTypeDef* hdma) const noexcept;
};

#endif // HAL modules
#endif
