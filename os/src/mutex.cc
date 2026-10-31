#include "mutex.hpp"
#include "cmsis_os2.h"
#include "thread.hpp"
#include <cstdint>

namespace os {

mutex::mutex(osMutexAttr_t *attrs) : attrs(attrs) {
    handle = osMutexNew(this->attrs);
}

mutex::~mutex() {
    osMutexDelete(handle);
}

void mutex::lock() const {
    // Block until the mutex is ours. The previous implementation used a zero
    // timeout and ignored the result, so lock() could return without owning
    // the mutex and the following unlock() released someone else's lock.
    osMutexAcquire(handle, osWaitForever);
}

bool mutex::try_lock(uint32_t timeout) const {
    return osMutexAcquire(handle, timeout) == osOK;
}

void mutex::unlock() const {
    osMutexRelease(handle);
}

const char *mutex::name() const {
    return osMutexGetName(handle);
}

thread_impl mutex::owner() const {
    return thread_impl(osMutexGetOwner(handle));
}

}
