#include "i2c_bus.hpp"
#include "cmsis_os2.h"

os::mutex& i2c_internal_bus_mutex() {
    // Priority inheritance: the bus is shared between threads of different priority.
    static osMutexAttr_t attrs = {
        .name = "i2c-internal",
        .attr_bits = osMutexPrioInherit,
        .cb_mem = nullptr,
        .cb_size = 0,
    };
    // First call happens in controller_system_init() before the worker threads exist,
    // so the (non thread-safe, -fno-threadsafe-statics) initialisation is race free.
    static os::mutex bus(&attrs);
    return bus;
}
