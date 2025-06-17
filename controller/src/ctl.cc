#include <cstdio>

#include "board.h"
#include "cmsis_os2.h"
#include "config.hpp"
#include "thread.hpp"

#include "log.hpp"

extern "C" void controller_system_init(const board_config *config) {
    os::init(LOG_LEVEL);

    os::info("Booting up\n");

    for (uint32_t i=0;; ++i) {
        os::info("tick %d\n", i);
        osDelay(1000);
    }
}
