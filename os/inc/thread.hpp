#ifndef __THREAD_HPP__
#define __THREAD_HPP__

#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#include "cmsis_os2.h"

#include "flags.hpp"

namespace os {

class thread_impl {
    osThreadId_t handle;

    thread_impl();
    thread_impl(osThreadId_t handle);

    void _set_thread_handle(osThreadId_t handle);
public:
    static thread_impl spawn(osThreadAttr_t *attr, osThreadFunc_t func, void *arg);

    const char *name() const;
    osThreadState_t state() const;
    osStatus_t set_priority(osPriority_t priority) const;
    osPriority_t get_priority() const;
    osStatus_t suspend() const;
    osStatus_t resume() const;
    osStatus_t detach() const;
    osStatus_t join() const;
    uint32_t stack_size() const;
    uint32_t stack_space() const;

    static osStatus_t yield();
    static thread_impl current();
    static void exit();

    template<typename ... T>
    uint32_t flag_set(T&&... t) {
        auto val = (t | ...);
        return osThreadFlagsSet(handle, static_cast<uint32_t>(val));
    }

    template<typename ... T>
    static uint32_t flag_clear(T&&... t) {
        auto val = (t | ...);
        return osThreadFlagsClear(static_cast<uint32_t>(val));
    }

    static uint32_t flags();

    template<typename ... T>
    static uint32_t flag_wait(OsWaitFlagsOpt opt, uint32_t timeout, T&&... t) {
        auto val = (t | ...);
        return osThreadFlagsWait(static_cast<uint32_t>(val), static_cast<uint32_t>(opt), timeout);
    }

    operator bool() const;

    template<typename F, typename ... T>
    friend class thread;
    friend class thread_enumerate;
    friend class mutex;
};

class thread_enumerate {
    std::vector<osThreadId_t> handles;

    thread_enumerate();
public:
    std::size_t size() const;
    thread_impl operator[](std::size_t n) const;
};

template<typename F, typename ... T>
class thread : thread_impl {
    struct Invocable {
        F f;
        std::tuple<T...> args;
    };

    std::unique_ptr<Invocable> closure;
    osThreadAttr_t *attrs;

    static void start(void *arg) {
        Invocable *closure = 
            reinterpret_cast<Invocable *>(arg);
        std::apply(closure->f, closure->args);
        thread_impl::exit();
    }

    thread(thread_impl handle) : thread_impl(handle) {}
    thread(const thread& ) = delete;
    thread& operator=(const thread& ) = delete;
public:
    thread(osThreadAttr_t *thread_attrs, F&& f, T&&... t) : attrs(thread_attrs) {
        closure = std::make_unique<Invocable>(
            Invocable {
                f,
                std::make_tuple(t...)
            }
        );

        osThreadId_t handle = osThreadNew(
            &thread::start, 
            closure.get(),
            attrs
        );

        _set_thread_handle(handle);
    }

    thread(F&& f, T&&... t)
        : thread(nullptr, std::forward<F&&>(f), std::forward<T&&>(t)...) {}
};

}


#endif
