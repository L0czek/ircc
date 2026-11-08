#include "ui/input.hpp"

#include <cstdlib>

namespace ui {

uint8_t keys_from_pcf8574(uint8_t port) noexcept {
    const uint8_t low = static_cast<uint8_t>(~port);   // keys pull the lines low
    uint8_t mask = 0;
    if (low & 0x01) mask |= key_bit(Key::Push);
    if (low & 0x02) mask |= key_bit(Key::Center);
    if (low & 0x04) mask |= key_bit(Key::Down);
    if (low & 0x08) mask |= key_bit(Key::Left);
    if (low & 0x10) mask |= key_bit(Key::Right);
    if (low & 0x20) mask |= key_bit(Key::Up);
    if (low & 0x80) mask |= key_bit(Key::Shutdown);
    return mask;
}

// ---------------------------------------------------------------------------------------

void EventQueue::push(const Event& e) noexcept {
    if (count_ == kSize) {
        head_ = (head_ + 1) % kSize;
        --count_;
    }
    events_[(head_ + count_) % kSize] = e;
    ++count_;
}

bool EventQueue::pop(Event& e) noexcept {
    if (count_ == 0) {
        return false;
    }
    e = events_[head_];
    head_ = (head_ + 1) % kSize;
    --count_;
    return true;
}

// ---------------------------------------------------------------------------------------

uint32_t KeyTracker::held_ms(Key k, uint32_t now_ms) const noexcept {
    return held(k) ? now_ms - down_at_[static_cast<size_t>(k)] : 0;
}

void KeyTracker::update(uint8_t raw_mask, uint32_t now_ms, EventQueue& out) noexcept {
    // Debounce: a new mask is accepted once it was seen unchanged for debounce_ms
    if (raw_mask != candidate_) {
        candidate_ = raw_mask;
        candidate_since_ = now_ms;
    }
    if (candidate_ != stable_ && now_ms - candidate_since_ >= timing_.debounce_ms) {
        const uint8_t changed = static_cast<uint8_t>(candidate_ ^ stable_);
        for (uint8_t i = 0; i < static_cast<uint8_t>(Key::Count); ++i) {
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            if (!(changed & bit)) {
                continue;
            }
            const Key k = static_cast<Key>(i);
            if (candidate_ & bit) {
                down_at_[i] = now_ms;
                next_repeat_[i] = now_ms + timing_.repeat_delay_ms;
                long_sent_ = static_cast<uint8_t>(long_sent_ & ~bit);
                out.push(Event{EventType::Press, k});
            } else {
                if (!(long_sent_ & bit)) {
                    out.push(Event{EventType::Click, k});
                }
                out.push(Event{EventType::Release, k});
                long_sent_ = static_cast<uint8_t>(long_sent_ & ~bit);
            }
        }
        stable_ = candidate_;
    }

    // Timers on held keys
    for (uint8_t i = 0; i < static_cast<uint8_t>(Key::Count); ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if (!(stable_ & bit)) {
            continue;
        }
        const Key k = static_cast<Key>(i);
        if (is_nav(k)) {
            if (static_cast<int32_t>(now_ms - next_repeat_[i]) >= 0) {
                out.push(Event{EventType::Repeat, k});
                next_repeat_[i] = now_ms + timing_.repeat_ms;
            }
        } else if (!(long_sent_ & bit) && now_ms - down_at_[i] >= timing_.long_ms) {
            long_sent_ = static_cast<uint8_t>(long_sent_ | bit);
            out.push(Event{EventType::Long, k});
        }
    }
}

// ---------------------------------------------------------------------------------------

void EncoderAccel::update(int32_t detents, uint32_t now_ms, EventQueue& out) noexcept {
    // Age the 20 ms buckets
    const uint32_t slot = now_ms / 20;
    const uint32_t last = bucket_time_;
    if (slot != last) {
        const uint32_t steps = slot - last;
        for (uint32_t s = 0; s < steps && s < buckets_.size(); ++s) {
            for (size_t i = buckets_.size() - 1; i > 0; --i) {
                buckets_[i] = buckets_[i - 1];
            }
            buckets_[0] = 0;
        }
        bucket_time_ = slot;
    }
    if (detents == 0) {
        return;
    }
    const uint32_t mag = static_cast<uint32_t>(std::abs(detents));
    const uint32_t sum = buckets_[0] + mag;
    buckets_[0] = static_cast<uint8_t>(sum > 255u ? 255u : sum);
    uint32_t speed = 0;
    for (uint8_t b : buckets_) {
        speed += b;
    }
    uint8_t accel = 1;
    if (config_.enabled) {
        if (speed >= config_.faster) {
            accel = 100;
        } else if (speed >= config_.fast) {
            accel = 10;
        }
    }
    int32_t d = config_.reverse ? -detents : detents;
    if (d > 32767) d = 32767;
    if (d < -32767) d = -32767;
    out.push(Event{EventType::Turn, Key::Count, static_cast<int16_t>(d), accel});
}

} // namespace ui
