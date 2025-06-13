#include "board.h"
#include <cstdio>

extern "C" void controller_system_init(const board_config *config) {
    printf("Hello world\n");
}
