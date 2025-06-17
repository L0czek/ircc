#include "mutex.hpp"
#include "cmsis_os2.h"
#include "thread.hpp"
#include <cstdint>
#include <memory>

namespace os {

mutex::mutex(std::unique_ptr<osMutexAttr_t> attrs) : attrs(std::move(attrs)) {
    handle = osMutexNew(this->attrs.get());
}

mutex::~mutex() {
    osMutexDelete(handle);
}

void mutex::lock() const {
    osMutexAcquire(handle, 0);
}

bool mutex::try_unlock(uint32_t timeout) const {
    osStatus_t status = osMutexAcquire(handle, timeout);
    return status == osOK;
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
