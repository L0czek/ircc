#ifndef __SEM_HPP__
#define __SEM_HPP__

#include "cmsis_os2.h"
#include <cstddef>
#include <memory>
namespace os {

class semaphore {
    osSemaphoreId_t handle;
    std::unique_ptr<osSemaphoreAttr_t> attrs;
public:
    semaphore(uint32_t max_count, uint32_t start_count, std::unique_ptr<osSemaphoreAttr_t> attrs);
    semaphore(const semaphore &other) = delete;
    ~semaphore();

    void release(std::ptrdiff_t update = 1) const;
    void acquire() const;

    bool try_acquire() const;
    bool try_acquire_for(uint32_t timeout) const;

    const char *name() const;
    uint32_t count() const;
};

}

#endif 
