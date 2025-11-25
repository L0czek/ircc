#ifndef __QUEUE_HPP__
#define __QUEUE_HPP__

#include <cstdint>
#include "cmsis_os2.h"

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

}

#endif
