#ifndef __WS2812B_HPP__
#define __WS2812B_HPP__

#include "error.hpp"
#include "stm32g4xx_hal.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>

/**
 * @brief WS2812B (NeoPixel) LED chain on a timer PWM channel.
 *
 * Board: g_board_tim_ws2812b / g_board_tim_ws2812b_channel (TIM5_CH4, PE9 -> WS2812B DIN).
 *
 * Each bit is one PWM period at 800 kHz with a 0.35 us (0) or 0.8 us (1) high time;
 * start() reprograms PSC/ARR for that, so the CubeMX period value does not matter.
 *
 * Two output modes, chosen by start():
 *  - DMA: when CubeMX links a DMA channel to the timer channel (TIM5_CH4 request,
 *    memory to peripheral, normal mode, memory increment, word or half-word). show()
 *    returns immediately; the frame is sent in the background.
 *  - Polled: without a DMA link, show() writes each bit's compare value itself with
 *    interrupts masked (30 us per LED). Limited to kMaxPolledLeds; fine for the single
 *    on-board LED, but use DMA for longer chains.
 *
 * Wire HAL_TIM_PWM_PulseFinishedCallback to on_pulse_finished() (done in interrupts.cc).
 * Pixel setters and show() must be called from one thread.
 */
class Ws2812b {
public:
    struct Rgb {
        uint8_t r, g, b;
    };

    static constexpr uint32_t kBitHz = 800000;
    static constexpr uint32_t kT0HighNs = 350;
    static constexpr uint32_t kT1HighNs = 800;
    static constexpr uint32_t kResetUs = 300;           // WS2812B-V5 latch time (> 280 us)
    static constexpr size_t kTrailingSlots = 2;         // low periods after the data (see show())
    static constexpr size_t kMaxPolledLeds = 8;

    /** @brief DMA buffer words needed for `leds` LEDs */
    static constexpr size_t buffer_words(size_t leds) { return leds * 24u + kTrailingSlots; }

    /** @brief Pixel and DMA storage for N LEDs (static allocation) */
    template <size_t N>
    struct Storage {
        Rgb pixels[N];
        uint32_t dma[buffer_words(N)];
        static constexpr size_t count = N;
    };

    Ws2812b(TIM_HandleTypeDef* htim, uint32_t channel, Rgb* pixels, uint32_t* dma_buffer, size_t count) noexcept;

    template <size_t N>
    Ws2812b(TIM_HandleTypeDef* htim, uint32_t channel, Storage<N>& storage) noexcept
        : Ws2812b(htim, channel, storage.pixels, storage.dma, N) {}

    /** @brief Set the bit timing, pick DMA or polled output and send an all-off frame */
    std::expected<void, ControllerError> start() noexcept;

    size_t size() const noexcept { return count_; }
    bool uses_dma() const noexcept { return use_dma_; }

    void set(size_t index, Rgb color) noexcept;
    Rgb get(size_t index) const noexcept { return index < count_ ? pixels_[index] : Rgb{0, 0, 0}; }
    void fill(Rgb color) noexcept;
    void clear() noexcept { fill({0, 0, 0}); }

    /** @brief Global scale applied in show() (255 = full); the LEDs are very bright at 5 V */
    void set_brightness(uint8_t brightness) noexcept { brightness_ = brightness; }
    uint8_t brightness() const noexcept { return brightness_; }

    /**
     * @brief Send the pixels. Waits for the latch gap after the previous frame.
     * @return Peripheral_Busy if a DMA frame is still being sent
     */
    std::expected<void, ControllerError> show() noexcept;

    /** @brief True while a DMA frame is being sent */
    bool busy() const noexcept { return busy_.load(std::memory_order_acquire); }

    /** @brief Dispatch from HAL_TIM_PWM_PulseFinishedCallback (ISR) */
    static void on_pulse_finished(TIM_HandleTypeDef* htim) noexcept;

    /** @brief Scale an 8-bit value by brightness (also usable for simple dimming) */
    static uint8_t scale(uint8_t value, uint8_t brightness) noexcept {
        return static_cast<uint8_t>((static_cast<uint16_t>(value) * (brightness + 1u)) >> 8);
    }

private:
    void encode() noexcept;
    void send_polled() noexcept;
    void finish_dma() noexcept;
    void wait_latch() const noexcept;

    TIM_HandleTypeDef* htim_;
    uint32_t channel_;
    Rgb* pixels_;
    uint32_t* dma_;
    size_t count_;
    uint8_t brightness_ = 64;
    bool use_dma_ = false;
    bool halfword_ = false;            // DMA memory width is 16 bits
    bool started_ = false;
    uint32_t t0_ = 0;                   // compare values for 0 and 1 bits
    uint32_t t1_ = 0;
    std::atomic<bool> busy_{false};
    std::atomic<uint32_t> done_ms_{0};  // HAL tick when the last frame ended (latch gap)

    static constexpr size_t kMaxInstances = 2;
    static Ws2812b* instances_[kMaxInstances];
};

#endif
