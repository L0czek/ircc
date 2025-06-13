cmake_minimum_required(VERSION 3.22)

include("${CMAKE_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/utils.cmake")

project(ircc)
enable_language(C CXX ASM)

set(board "stm32g474re_nucleo_board")

if (${board} STREQUAL "stm32g474re_nucleo_board")
    use_cubemx_target(${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board)
else ()
    message(SEND_ERROR "Invalid target board")
endif ()
