#ifndef __MUTEX_HPP__
#define __MUTEX_HPP__

#include "cmsis_os2.h"

#include "thread.hpp"

#include <memory>

namespace os {

class mutex {
    osMutexId_t handle;
    std::unique_ptr<osMutexAttr_t> attrs;
public:
    mutex(std::unique_ptr<osMutexAttr_t> attrs);
    mutex(const mutex &) = delete;
    ~mutex();

    void lock() const;
    bool try_unlock(uint32_t timeout = 1) const;
    void unlock() const;

    const char *name() const;
    thread_impl owner() const;
};

}

#endif
