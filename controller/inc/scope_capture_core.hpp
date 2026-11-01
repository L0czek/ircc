#ifndef __SCOPE_CAPTURE_CORE_HPP__
#define __SCOPE_CAPTURE_CORE_HPP__

#include <array>
#include <cstdint>
#include <optional>

/**
 * @brief Hardware-independent bookkeeping of a pre-trigger oscilloscope capture.
 *
 * Model (all counts are in samples of one stream; every stream gets exactly one
 * sample per sample-clock tick, so sample indices are common to all streams):
 *
 *   ADC --DMA--> SRAM ring (ring_samples, circular) --burst--> PSRAM region (region_samples, circular)
 *
 * - The producer (circular DMA) reports progress at half-ring granularity through
 *   on_produced() (half-transfer / transfer-complete events). Between two reports the
 *   DMA may already be up to half a ring further, so data in the half the DMA is about
 *   to overwrite must already be consumed: overrun if produced - consumed > ring / 2.
 * - The consumer moves bursts of burst_samples from the SRAM ring to PSRAM. Bursts are
 *   aligned, so they never wrap the SRAM ring or the PSRAM region.
 * - A trigger at absolute sample index T ends the capture once T + post samples are
 *   produced; production is then stopped, the rest drained, and the PSRAM region holds
 *   the newest region_samples samples (including >= pre samples before T).
 *
 * Not thread safe: the owner serialises calls (same interrupt priority / critical
 * section).
 */
namespace scope {

constexpr uint32_t kMaxStreams = 4;

enum class State : uint8_t {
    Idle,          // not armed
    PreTrigger,    // recording, waiting for a trigger
    PostTrigger,   // triggered, recording the post-trigger samples
    Stopping,      // production stopped, draining SRAM rings to PSRAM
    Done,          // capture complete, window() valid
    Error,
};

enum class Fault : uint8_t {
    None,
    Overrun,      // producer overwrote data not yet written to PSRAM
    WriteError,   // PSRAM burst failed
    DmaError,     // sample DMA transfer error
    Aborted,      // stopped by the user before completion
};

struct CoreConfig {
    uint32_t num_streams = 0;
    uint32_t ring_samples = 0;     // SRAM ring per stream; even multiple of burst_samples
    uint32_t burst_samples = 0;    // PSRAM burst; even
    uint32_t region_samples = 0;   // PSRAM ring per stream; multiple of burst_samples
    uint32_t pre_samples = 0;
    uint32_t post_samples = 0;
    bool require_full_pretrigger = true;   // ignore triggers until pre_samples recorded
};

/** @brief One SRAM -> PSRAM transfer chosen by next_burst() */
struct Burst {
    uint32_t stream;
    uint32_t ring_offset;     // first sample in the SRAM ring
    uint32_t region_offset;   // first sample in the stream's PSRAM region
    uint32_t samples;         // even
};

/** @brief Valid data after Done, as absolute sample indices */
struct Window {
    uint64_t trigger;   // T
    uint64_t first;     // oldest sample still in PSRAM
    uint64_t end;       // one past the last requested sample (T + post)
};

class CaptureCore {
public:
    /** @return false (and Idle) if the configuration is inconsistent */
    bool configure(const CoreConfig& config) noexcept;
    const CoreConfig& config() const noexcept { return config_; }

    void arm() noexcept;

    /** @brief Producer progress (monotonic, half-ring granular) of one stream */
    void on_produced(uint32_t stream, uint64_t produced) noexcept;

    /** @brief Trigger at absolute sample index; @return true if accepted */
    bool trigger(uint64_t sample_index, uint8_t source) noexcept;

    /** @brief True once every requested post-trigger sample has been produced */
    bool should_stop_production() const noexcept;

    /** @brief Production stopped; exact final sample counts per stream */
    void production_stopped(const std::array<uint64_t, kMaxStreams>& final_produced) noexcept;

    /** @brief Next burst to write, or none; marks it in flight */
    std::optional<Burst> next_burst() noexcept;

    /** @brief The in-flight burst completed (ok) or failed */
    void burst_done(bool ok) noexcept;

    void fail(Fault fault) noexcept;
    void abort() noexcept { fail(Fault::Aborted); }

    State state() const noexcept { return state_; }
    Fault fault() const noexcept { return fault_; }
    bool burst_in_flight() const noexcept { return in_flight_; }
    uint8_t trigger_source() const noexcept { return trigger_source_; }
    uint64_t produced(uint32_t stream) const noexcept { return produced_[stream]; }
    uint64_t consumed(uint32_t stream) const noexcept { return consumed_[stream]; }

    /** @brief Data window of a finished capture (state Done) */
    Window window() const noexcept;

private:
    CoreConfig config_{};
    State state_ = State::Idle;
    Fault fault_ = Fault::None;
    std::array<uint64_t, kMaxStreams> produced_{};   // committed producer counts
    std::array<uint64_t, kMaxStreams> consumed_{};   // samples written to PSRAM (incl. padding)
    std::array<uint64_t, kMaxStreams> final_{};      // exact counts after production stopped
    uint64_t trigger_ = 0;
    uint8_t trigger_source_ = 0;
    bool in_flight_ = false;
    Burst current_{};
    uint32_t next_stream_ = 0;                       // round-robin start

    uint64_t available(uint32_t stream) const noexcept;
    void check_done() noexcept;
};

} // namespace scope

#endif
