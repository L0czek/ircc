#ifndef __RING_BUFFER_HPP__
#define __RING_BUFFER_HPP__

#include <array>
#include <cstddef>
#include <utility>

namespace os {

template<typename T, std::size_t N>
class RingBuffer {
public:
    static_assert(N > 0, "RingBuffer capacity must be > 0");

    using value_type = T;
    using size_type  = std::size_t;

private:
    std::array<T, N> buffer{};
    size_type head = 0;   // consumer index
    size_type tail = 0;   // producer index
    size_type mask = N - 1;

    // SPSC invariant: buffer full if next index == head
    constexpr size_type next(size_type i) const noexcept {
        return (i + 1) % N;
    }

public:
    // ---- Capacity ----
    constexpr bool empty() const noexcept {
        return head == tail;
    }

    constexpr bool full() const noexcept {
        return next(tail) == head;
    }

    constexpr size_type capacity() const noexcept { return N; }

    constexpr size_type size() const noexcept {
        if (tail >= head) return tail - head;
        return N - (head - tail);
    }

    // ---- Element access ----
    constexpr T& front() noexcept { return buffer[head]; }
    constexpr const T& front() const noexcept { return buffer[head]; }

    constexpr T& back() noexcept {
        size_type last = (tail == 0 ? N - 1 : tail - 1);
        return buffer[last];
    }
    constexpr const T& back() const noexcept {
        size_type last = (tail == 0 ? N - 1 : tail - 1);
        return buffer[last];
    }

    // ---- Push by copy/move ----
    // returns false if full
    constexpr bool push_back(const T& value) noexcept {
        size_type next_tail = next(tail);
        if (next_tail == head) return false;   // full
        buffer[tail] = value;
        tail = next_tail;
        return true;
    }

    constexpr bool push_back(T&& value) noexcept {
        size_type next_tail = next(tail);
        if (next_tail == head) return false;
        buffer[tail] = std::move(value);
        tail = next_tail;
        return true;
    }

    // ---- Push default-constructed T (no copy, no temp) ----
    // returns pointer or nullptr if full
    constexpr T* push_back_default() noexcept {
        size_type next_tail = next(tail);
        if (next_tail == head) return nullptr; // full

        // default construct in-place without any copying
        buffer[tail] = T{};
        T* ref = &buffer[tail];

        tail = next_tail;
        return ref;
    }

    // ---- Pop ----
    constexpr bool pop_front() noexcept {
        if (empty()) return false;
        head = next(head);
        return true;
    }

    // ---- Clear ----
    constexpr void clear() noexcept {
        head = 0;
        tail = 0;
    }
};

}

#endif 
