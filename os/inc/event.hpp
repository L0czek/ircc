#ifndef __EVENT_HPP__
#define __EVENT_HPP__

#include <memory>

#include "cmsis_os2.h"

#include "flags.hpp"

namespace os {

class event {
    osEventFlagsId_t handle;
    std::unique_ptr<osEventFlagsAttr_t> attrs;
public:
    event(std::unique_ptr<osEventFlagsAttr_t> attr);
    event();
    event(const event& ) = delete;
    ~event();

    template<typename ... T>
    uint32_t set(T... t) const {
        auto val = (static_cast<uint32_t>(t) | ...);
        return osEventFlagsSet(handle, val);
    }

    template<typename ... T>
    uint32_t clear(T... t) const {
        auto val = (static_cast<uint32_t>(t) | ...);
        return osEventFlagsClear(handle, val);
    }

    uint32_t get() const;

    template<typename ... T>
    uint32_t wait(OsWaitFlagsOpt opt, uint32_t timeout, T... t) const {
        auto val = (static_cast<uint32_t>(t) | ...);
        return osEventFlagsWait(handle, val, static_cast<uint32_t>(opt), timeout);
    }

    const char *name() const;
};

}

#endif

