#include "scope_capture_core.hpp"
#include <algorithm>

namespace scope {

bool CaptureCore::configure(const CoreConfig& c) noexcept {
    state_ = State::Idle;
    fault_ = Fault::None;
    const bool ok =
        c.num_streams >= 1 && c.num_streams <= kMaxStreams &&
        c.burst_samples >= 2 && (c.burst_samples % 2) == 0 &&
        c.ring_samples >= 2 * c.burst_samples && (c.ring_samples % (2 * c.burst_samples)) == 0 &&
        c.region_samples >= c.burst_samples && (c.region_samples % c.burst_samples) == 0 &&
        // Overshoot after the post-trigger end: up to half a ring before the stop is
        // noticed plus one more half in flight. Reserve a full ring so the requested
        // pre-trigger samples are never overwritten.
        static_cast<uint64_t>(c.pre_samples) + c.post_samples + c.ring_samples <= c.region_samples;
    config_ = ok ? c : CoreConfig{};
    return ok;
}

void CaptureCore::arm() noexcept {
    produced_.fill(0);
    consumed_.fill(0);
    final_.fill(0);
    trigger_ = 0;
    trigger_source_ = 0;
    in_flight_ = false;
    next_stream_ = 0;
    fault_ = Fault::None;
    state_ = config_.num_streams ? State::PreTrigger : State::Idle;
}

uint64_t CaptureCore::available(uint32_t s) const noexcept {
    const uint64_t limit = (state_ == State::Stopping) ? final_[s] : produced_[s];
    return limit > consumed_[s] ? limit - consumed_[s] : 0;
}

void CaptureCore::on_produced(uint32_t s, uint64_t produced) noexcept {
    if (s >= config_.num_streams || (state_ != State::PreTrigger && state_ != State::PostTrigger)) {
        return;
    }
    produced_[s] = std::max(produced_[s], produced);
    // The DMA now writes the half after `produced`, which holds samples
    // [produced - ring, produced - ring/2): they must already be in PSRAM.
    if (produced_[s] > consumed_[s] + config_.ring_samples / 2) {
        fail(Fault::Overrun);
    }
}

bool CaptureCore::trigger(uint64_t index, uint8_t source) noexcept {
    if (state_ != State::PreTrigger) {
        return false;
    }
    if (config_.require_full_pretrigger && index < config_.pre_samples) {
        return false;   // holdoff: not enough history yet
    }
    trigger_ = index;
    trigger_source_ = source;
    state_ = State::PostTrigger;
    return true;
}

bool CaptureCore::should_stop_production() const noexcept {
    if (state_ != State::PostTrigger) {
        return false;
    }
    const uint64_t end = trigger_ + config_.post_samples;
    for (uint32_t s = 0; s < config_.num_streams; ++s) {
        if (produced_[s] < end) {
            return false;
        }
    }
    return true;
}

void CaptureCore::production_stopped(const std::array<uint64_t, kMaxStreams>& final_produced) noexcept {
    if (state_ != State::PostTrigger && state_ != State::PreTrigger) {
        return;
    }
    for (uint32_t s = 0; s < config_.num_streams; ++s) {
        final_[s] = std::max(final_produced[s], produced_[s]);
        // Final position may be up to a ring ahead of the last committed half.
        if (final_[s] > consumed_[s] + config_.ring_samples) {
            fail(Fault::Overrun);
            return;
        }
    }
    if (state_ == State::PreTrigger) {
        // Stopped without a trigger (user stop): nothing to keep.
        fail(Fault::Aborted);
        return;
    }
    state_ = State::Stopping;
    check_done();
}

std::optional<Burst> CaptureCore::next_burst() noexcept {
    if (in_flight_ || (state_ != State::PreTrigger && state_ != State::PostTrigger && state_ != State::Stopping)) {
        return std::nullopt;
    }
    const bool draining = (state_ == State::Stopping);
    for (uint32_t i = 0; i < config_.num_streams; ++i) {
        const uint32_t s = (next_stream_ + i) % config_.num_streams;
        const uint64_t avail = available(s);
        uint32_t n = 0;
        if (avail >= config_.burst_samples) {
            n = config_.burst_samples;
        } else if (draining && avail > 0) {
            // Final partial burst; round up to the 2-sample (4-byte) DMA granularity. The
            // padding sample lies after final_[s] and is never reported as valid.
            n = static_cast<uint32_t>(avail + (avail & 1u));
        }
        if (n == 0) {
            continue;
        }
        current_.stream = s;
        current_.ring_offset = static_cast<uint32_t>(consumed_[s] % config_.ring_samples);
        current_.region_offset = static_cast<uint32_t>(consumed_[s] % config_.region_samples);
        current_.samples = n;
        in_flight_ = true;
        next_stream_ = (s + 1) % config_.num_streams;
        return current_;
    }
    return std::nullopt;
}

void CaptureCore::burst_done(bool ok) noexcept {
    if (!in_flight_) {
        return;
    }
    in_flight_ = false;
    if (!ok) {
        fail(Fault::WriteError);
        return;
    }
    consumed_[current_.stream] += current_.samples;
    check_done();
}

void CaptureCore::check_done() noexcept {
    if (state_ != State::Stopping || in_flight_) {
        return;
    }
    for (uint32_t s = 0; s < config_.num_streams; ++s) {
        if (consumed_[s] < final_[s]) {
            return;
        }
    }
    state_ = State::Done;
}

void CaptureCore::fail(Fault f) noexcept {
    if (state_ == State::Done || state_ == State::Error || state_ == State::Idle) {
        return;
    }
    fault_ = f;
    state_ = State::Error;
}

Window CaptureCore::window() const noexcept {
    Window w{};
    if (state_ != State::Done) {
        return w;
    }
    w.trigger = trigger_;
    w.end = trigger_ + config_.post_samples;
    // Oldest sample still present in every stream's PSRAM region.
    uint64_t newest = 0;
    for (uint32_t s = 0; s < config_.num_streams; ++s) {
        newest = std::max(newest, consumed_[s]);
    }
    w.first = newest > config_.region_samples ? newest - config_.region_samples : 0;
    return w;
}

} // namespace scope
