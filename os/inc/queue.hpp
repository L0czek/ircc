#ifndef __QUEUE_HPP__
#define __QUEUE_HPP__

#include <cstdint>
#include <ios>
#include <mutex>

#include "cmsis_os2.h"

#include "mutex.hpp"
#include "ring_buffer.hpp"

namespace os {

template<typename T>
class queue {
    osMessageQueueId_t handle;
    osMessageQueueAttr_t *attrs;
public:
    queue(uint32_t count, osMessageQueueAttr_t *attrs) : attrs(attrs) {
        handle = osMessageQueueNew(count, sizeof(T), this->attrs);
    }
    queue(const queue &other) = delete;

    ~queue() {
        osMessageQueueDelete(handle);
    }

    std::size_t size() const {
        return osMessageQueueGetCount(handle);
    }

    std::size_t capacity() const {
        return osMessageQueueGetCapacity(handle);
    }

    std::size_t space_left() const {
        return osMessageQueueGetSpace(handle);
    }

    void clear() const {
        osMessageQueueReset(handle);
    }

    osStatus_t put(const T &t, uint8_t prio, uint32_t timeout) const {
        auto ptr = reinterpret_cast<const void *>(&t);
        return osMessageQueuePut(handle, ptr, prio, timeout);
    }

    osStatus_t get(T &t, uint8_t &prio, uint32_t timeout) const {
        auto ptr = reinterpret_cast<void *>(&t);
        return osMessageQueueGet(handle, ptr, &prio, timeout);
    }

    const char *name() const {
        return osMessageQueueGetName(handle);
    }
};

template<typename T, std::size_t N>
class fixed_queue :public queue<T> {
    RingBuffer<T, N> buffer;
    mutex lock;
public:
    fixed_queue(osMessageQueueAttr_t *attrs) :queue<T>(N, attrs), lock(nullptr) { }

    osStatus_t put(T t) {
        std::lock_guard guard(lock);

        if (!buffer.push_back(std::move(t)))
            return osErrorResource;

        return queue<T>::put(buffer.back(), 0, 0);
    }

    osStatus_t get(T &t) {
        std::lock_guard guard(lock);
        std::uint8_t prio = 0;
        osStatus_t status = queue<T>::get(t, prio, 0);

        if (status != osOK)
            return status;

        buffer.pop_front();

        return osOK;
    }
};

}

#endif
