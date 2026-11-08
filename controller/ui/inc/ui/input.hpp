#ifndef UI_INPUT_HPP
#define UI_INPUT_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace ui {

/** @brief Front panel keys (bit positions in a key mask) */
enum class Key : uint8_t {
    Up,
    Down,
    Left,
    Right,
    Center,     // joystick centre
    Push,       // encoder push switch
    Shutdown,   // SHUTDOWN key: click = soft stop, hold = power off
    Count,
};

constexpr uint8_t key_bit(Key k) noexcept { return static_cast<uint8_t>(1u << static_cast<uint8_t>(k)); }
constexpr bool is_nav(Key k) noexcept { return k == Key::Up || k == Key::Down || k == Key::Left || k == Key::Right; }
constexpr bool is_select(Key k) noexcept { return k == Key::Center || k == Key::Push; }

/**
 * @brief PCF8574 port value (active-low keys) to a key mask.
 *        IO board: P0 encoder push, P1 centre, P2 down, P3 left, P4 right, P5 up, P7 SHUTDOWN.
 */
uint8_t keys_from_pcf8574(uint8_t port) noexcept;

enum class EventType : uint8_t {
    Press,     // key went down
    Release,   // key went up (after Click/Long)
    Click,     // released before the long-press time
    Long,      // held for the long-press time (select keys and SHUTDOWN only)
    Repeat,    // auto repeat while held (navigation keys)
    Turn,      // encoder: delta detents, accel multiplier
};

struct Event {
    EventType type;
    Key key = Key::Count;
    int16_t delta = 0;     // Turn: detents (+ clockwise after the direction setting)
    uint8_t accel = 1;     // Turn: 1, 10 or 100 depending on speed

    bool is(EventType t, Key k) const noexcept { return type == t && key == k; }
    bool select_click() const noexcept { return type == EventType::Click && is_select(key); }
    bool select_long() const noexcept { return type == EventType::Long && is_select(key); }
};

/** @brief Fixed-size FIFO of events (oldest dropped when full) */
class EventQueue {
public:
    void push(const Event& e) noexcept;
    bool pop(Event& e) noexcept;
    bool empty() const noexcept { return count_ == 0; }
    void clear() noexcept { head_ = count_ = 0; }

private:
    static constexpr size_t kSize = 16;
    std::array<Event, kSize> events_{};
    size_t head_ = 0;
    size_t count_ = 0;
};

struct InputTiming {
    uint16_t debounce_ms = 10;
    uint16_t long_ms = 600;
    uint16_t repeat_delay_ms = 400;
    uint16_t repeat_ms = 110;
};

/** @brief Debounces a raw key mask and produces Press/Release/Click/Long/Repeat events */
class KeyTracker {
public:
    explicit KeyTracker(const InputTiming& timing = {}) noexcept : timing_(timing) {}

    /** @brief New raw sample (also call periodically with the last sample for timers) */
    void update(uint8_t raw_mask, uint32_t now_ms, EventQueue& out) noexcept;

    uint8_t pressed() const noexcept { return stable_; }
    bool held(Key k) const noexcept { return (stable_ & key_bit(k)) != 0; }
    uint32_t held_ms(Key k, uint32_t now_ms) const noexcept;

private:
    InputTiming timing_;
    uint8_t stable_ = 0;
    uint8_t candidate_ = 0;
    uint32_t candidate_since_ = 0;
    std::array<uint32_t, static_cast<size_t>(Key::Count)> down_at_{};
    std::array<uint32_t, static_cast<size_t>(Key::Count)> next_repeat_{};
    uint8_t long_sent_ = 0;
};

/**
 * @brief Encoder detents to Turn events with a speed-dependent multiplier: x1, x10 at
 *        >= fast detents per 100 ms, x100 at >= faster detents per 100 ms.
 */
class EncoderAccel {
public:
    struct Config {
        bool reverse = false;
        bool enabled = true;
        uint8_t fast = 5;
        uint8_t faster = 15;
    };

    void set_config(const Config& c) noexcept { config_ = c; }
    const Config& config() const noexcept { return config_; }

    /** @brief Detents moved since the last call (positive = clockwise) */
    void update(int32_t detents, uint32_t now_ms, EventQueue& out) noexcept;

private:
    Config config_{};
    // Detents in 20 ms buckets over the last 100 ms
    std::array<uint8_t, 5> buckets_{};
    uint32_t bucket_time_ = 0;
};

} // namespace ui

#endif
