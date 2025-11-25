#ifndef __RING_BUFFER_HPP__
#define __RING_BUFFER_HPP__

#include <array>
#include <cstddef>
#include <utility>
#include <optional>
#include <functional>

namespace os {

template<typename T, std::size_t N>
class RingBuffer {
public:
    static_assert(N > 0, "RingBuffer capacity must be > 0");

    using value_type = T;
    using size_type  = std::size_t;
    using RefOpt     = std::optional<std::reference_wrapper<T>>;

private:
    std::array<T, N> buffer{};
    size_type head = 0;
    size_type tail = 0;

    constexpr size_type next(size_type i) const noexcept {
        return (i + 1) % N;
    }

public:
    // ---- Capacity ----
    constexpr bool empty() const noexcept { return head == tail; }
    constexpr bool full()  const noexcept { return next(tail) == head; }

    constexpr size_type size() const noexcept {
        if (tail >= head) return tail - head;
        return N - (head - tail);
    }

    constexpr size_type capacity() const noexcept { return N; }

    // ---- Access ----
    constexpr T& front() noexcept { return buffer[head]; }
    constexpr const T& front() const noexcept { return buffer[head]; }

    constexpr T& back() noexcept {
        return buffer[(tail == 0 ? N - 1 : tail - 1)];
    }
    constexpr const T& back() const noexcept {
        return buffer[(tail == 0 ? N - 1 : tail - 1)];
    }

    constexpr RefOpt push_back(const T& v) noexcept {
        size_type nt = next(tail);
        if (nt == head) 
            return std::nullopt;        // full

        buffer[tail] = v;               // copy
        T& ref = buffer[tail];

        tail = nt;
        return std::ref(ref);
    }

    constexpr RefOpt push_back(T&& v) noexcept {
        size_type nt = next(tail);
        if (nt == head)
            return std::nullopt;        // full

        buffer[tail] = std::move(v);    // move
        T& ref = buffer[tail];

        tail = nt;
        return std::ref(ref);
    }

    constexpr RefOpt push_back_emplace() noexcept {
        size_type nt = next(tail);
        if (nt == head)
            return std::nullopt;

        buffer[tail] = T{};             // default construct
        T& ref = buffer[tail];

        tail = nt;
        return std::ref(ref);
    }

    constexpr bool pop_front() noexcept {
        if (empty()) return false;
        head = next(head);
        return true;
    }

    constexpr void clear() noexcept {
        head = 0;
        tail = 0;
    }
};

}

#endif 
