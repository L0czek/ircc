#include "sem.hpp"
#include "cmsis_os2.h"
#include <cstddef>

namespace os {

semaphore::semaphore(uint32_t max_count, uint32_t start_count, std::unique_ptr<osSemaphoreAttr_t> attrs) : attrs(std::move(attrs)) {
    handle = osSemaphoreNew(max_count , start_count, this->attrs.get());
}

semaphore::~semaphore() {
    osSemaphoreDelete(handle);
}

const char *semaphore::name() const {
    return osSemaphoreGetName(handle);
}

uint32_t semaphore::count() const {
    return osSemaphoreGetCount(handle);
}

void semaphore::release(std::ptrdiff_t update) const {
    for (auto _=0; _ < update; ++_)
        osSemaphoreRelease(handle);
}

void semaphore::acquire() const {
    osSemaphoreAcquire(handle, 0);
}

bool semaphore::try_acquire() const {
    osStatus_t status = osSemaphoreAcquire(handle, 1);
    return status == osOK;
}

bool semaphore::try_acquire_for(uint32_t timeout) const {
    osStatus_t status = osSemaphoreAcquire(handle, timeout);
    return status == osOK;
}

}
