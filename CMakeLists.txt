cmake_minimum_required(VERSION 3.22)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include("${CMAKE_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/utils.cmake")

project(ircc)
enable_language(C CXX ASM)

set(CMAKE_BUILD_TYPE Debug)
set(board "stm32g474re_nucleo_board")
set(use_semihosting TRUE BOOL)
set(openocd_config "${CMAKE_BINARY_DIR}/openocd.cfg")
set(gdb_port 3333)
set(BOARD_HEADERS_INC ${CMAKE_SOURCE_DIR}/headers)
set(EXTERNAL_SOURCE_DIR ${CMAKE_SOURCE_DIR}/external)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/external/nanopb/extra)

find_package(Nanopb REQUIRED)
add_subdirectory(protocol)

if (${board} STREQUAL "stm32g474re_nucleo_board")
    message(STATUS "Using stm32g474re_nucleo_board")
    use_cubemx_target(${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board)
    set(target_name stm32g474re_nucleo_board)
    set(MCU STM32G474xx)
    set(chip_family stm32g4x)
    set(programmer_name stlink)
    set(output_elf "${CMAKE_BINARY_DIR}/target/stm32g474re_nucleo_board/stm32g474re_nucleo_board.elf")
else ()
    message(SEND_ERROR "Invalid target board")
endif ()

get_target_property(HAL_INCLUDE_DIRS stm32cubemx INTERFACE_INCLUDE_DIRECTORIES)

add_subdirectory(${EXTERNAL_SOURCE_DIR})

add_subdirectory(os)
add_subdirectory(controller)

target_link_libraries(${target_name} ctl)

configure_openocd()
configure_gdb()
link_compile_commands()

include(ExternalProject)
ExternalProject_Add(host
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/host
    BINARY_DIR ${CMAKE_BINARY_DIR}/host
    CMAKE_ARGS
        -DROOT_DIR=${CMAKE_SOURCE_DIR}
        -DPROTOCOL_SOURCES=${PROTOCOL_SOURCES}
    INSTALL_COMMAND
    ${CMAKE_COMMAND} -E copy
        ./hostctl.elf
        ${CMAKE_BINARY_DIR}/
)

