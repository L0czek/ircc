cmake_minimum_required(VERSION 3.22)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include("${CMAKE_SOURCE_DIR}/cmake/gcc-arm-none-eabi.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/utils.cmake")

project(ircc)
enable_language(C CXX ASM)

set(CMAKE_BUILD_TYPE Debug)
set(board "stm32g474re_nucleo_board" CACHE STRING "Target board")
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
elseif (${board} STREQUAL "resonant_converter_controller")
    message(STATUS "Using resonant_converter_controller")
    use_cubemx_target(${CMAKE_SOURCE_DIR}/target/resonant_converter_controller)
    set(target_name resonant_converter_controller)
    set(MCU STM32G474xx)
    set(chip_family stm32g4x)
    set(programmer_name stlink)
    set(output_elf "${CMAKE_BINARY_DIR}/target/resonant_converter_controller/resonant_converter_controller.elf")
else ()
    message(SEND_ERROR "Invalid target board")
endif ()

get_target_property(HAL_INCLUDE_DIRS stm32cubemx INTERFACE_INCLUDE_DIRECTORIES)

add_subdirectory(${EXTERNAL_SOURCE_DIR})

# Export TCLAP include dir for host build
set(TCLAP_INCLUDE_DIR ${EXTERNAL_SOURCE_DIR}/tclap-1.4.0-rc2/include CACHE PATH "TCLAP include directory")

add_subdirectory(os)
add_subdirectory(controller)

target_link_libraries(${target_name} ctl)

# Build host utilities - use ExternalProject_Add to use native compiler
# The host project needs native x86-64 compiler for ftxui, not arm-none-eabi
include(ExternalProject)

# Set USE_EMULATOR for host build (can be overridden at cmake config time)
if(NOT DEFINED USE_EMULATOR)
    set(USE_EMULATOR 1)
endif()

ExternalProject_Add(host
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/host
    BINARY_DIR ${CMAKE_BINARY_DIR}/host
    CMAKE_ARGS
        -DROOT_DIR=${CMAKE_SOURCE_DIR}
        -DPROTOCOL_SOURCES=${PROTOCOL_SOURCES}
        -DPROTOCOL_INC=${PROTOCOL_INC}
        -DTCLAP_INCLUDE_DIR=${TCLAP_INCLUDE_DIR}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_SYSTEM_NAME=Linux
        -DCMAKE_SYSTEM_PROCESSOR=x86_64
        -DUSE_EMULATOR=${USE_EMULATOR}
    INSTALL_COMMAND
    ${CMAKE_COMMAND} -E copy
        ./hostctl.elf
        ${CMAKE_BINARY_DIR}/
)

# Add custom target to build test executables
add_custom_target(test_host
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/host --target test_commands
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/host --target test_emulator
    DEPENDS host
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

configure_openocd()
configure_gdb()
link_compile_commands()

