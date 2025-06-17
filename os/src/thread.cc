#include "thread.hpp"
#include "cmsis_os2.h"

namespace os {

thread_impl::thread_impl() {}

thread_impl::thread_impl(osThreadId_t handle)
    : handle(handle) {}

thread_impl thread_impl::spawn(osThreadAttr_t *attr, osThreadFunc_t func, void *arg) {
    return thread_impl(osThreadNew(func, arg, attr));
}

const char *thread_impl::name() const {
    return osThreadGetName(handle);
}

osThreadState_t thread_impl::state() const {
    return osThreadGetState(handle);
}

osStatus_t thread_impl::set_priority(osPriority_t priority) const {
    return osThreadSetPriority(handle, priority);
}

osPriority_t thread_impl::get_priority() const {
    return osThreadGetPriority(handle);
}

osStatus_t thread_impl::suspend() const {
    return osThreadSuspend(handle);
}

osStatus_t thread_impl::resume() const {
    return osThreadResume(handle);
}

osStatus_t thread_impl::detach() const {
    return osThreadDetach(handle);
}

osStatus_t thread_impl::join() const {
    return osThreadJoin(handle);
}

uint32_t thread_impl::stack_size() const {
    return osThreadGetStackSize(handle);
}

uint32_t thread_impl::stack_space() const {
    return osThreadGetStackSpace(handle);
}

osStatus_t thread_impl::yield() {
    return osThreadYield();
}

thread_impl thread_impl::current() {
    return thread_impl(osThreadGetId());
}

void thread_impl::exit() {
    osThreadExit();
}

void thread_impl::_set_thread_handle(osThreadId_t handle) {
    this->handle = handle;
}

thread_enumerate::thread_enumerate() {
    std::size_t n = osThreadGetCount();
    handles.resize(n);
    std::size_t actual = osThreadEnumerate(handles.data(), n);

    if (n != actual)
        handles.resize(actual);
}

thread_impl thread_enumerate::operator[](std::size_t n) const {
    if (n < size())
        return thread_impl(handles[n]);
    else
        return thread_impl(nullptr);
}

std::size_t thread_enumerate::size() const {
    return handles.size();
}

}
