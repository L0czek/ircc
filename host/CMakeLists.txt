cmake_minimum_required(VERSION 3.22)

# ROOT_DIR is set when built via ExternalProject_Add, otherwise use CMAKE_SOURCE_DIR
if(NOT ROOT_DIR)
    set(ROOT_DIR "${CMAKE_SOURCE_DIR}")
endif()

include("${ROOT_DIR}/cmake/clang.cmake")
include("${ROOT_DIR}/cmake/utils.cmake")
project(host)
enable_language(C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set up nanopb module path for this subproject
list(APPEND CMAKE_MODULE_PATH "${ROOT_DIR}/external/nanopb/extra")
find_package(Nanopb REQUIRED)

# ftxui needs to be found
find_package(ftxui CONFIG REQUIRED)

# TCLAP - header-only, will be added from parent

# Set CMAKE_PREFIX_PATH for finding Boost and other native libraries
list(APPEND CMAKE_PREFIX_PATH "/usr/lib/x86_64-linux-gnu/cmake")

# Find Boost - host uses native x86-64 Boost libraries
find_package(Boost CONFIG REQUIRED COMPONENTS program_options log system)

nanopb_generate_cpp(TARGET protocol-amd64 ${PROTOCOL_SOURCES})

add_executable(hostctl
    ${ROOT_DIR}/controller/src/error_host.cc
    src/main.cc
    src/cli.cc
    src/connector.cc
    src/scope_client.cc
    src/periodic_task.cc
    src/tasks.cc
    src/tui.cc
    src/tui_commands.cc
    src/tui_views.cc
    src/tui_format.cc
)

# Define USE_EMULATOR for hostctl (can be overridden at cmake config time)
if(NOT DEFINED USE_EMULATOR)
    set(USE_EMULATOR 1)
endif()
target_compile_definitions(hostctl PRIVATE USE_EMULATOR=${USE_EMULATOR})

if(USE_EMULATOR)
    target_sources(hostctl PRIVATE
        src/hardware_emulator.cc
        src/emulator_connector.cc
        ${ROOT_DIR}/controller/src/applet_control.cc
    )
endif()

# Enable debug logging for TUI
add_compile_definitions(BOOST_LOG_TRIVIAL_DEBUG=1)

# Hardware emulator for testing without physical hardware
add_executable(test_emulator
    ${ROOT_DIR}/controller/src/error_host.cc
    test_emulator.cc
    src/hardware_emulator.cc
    ${ROOT_DIR}/controller/src/applet_control.cc
)

# Command tests for TUI
add_executable(test_commands
    ${ROOT_DIR}/controller/src/error_host.cc
    test_commands.cc
    src/cli.cc
    src/connector.cc
    src/scope_client.cc
    src/hardware_emulator.cc
    src/emulator_connector.cc
    ${ROOT_DIR}/controller/src/applet_control.cc
)

# Define USE_EMULATOR for test_emulator and test_commands (can be overridden at cmake config time)
if(NOT DEFINED USE_EMULATOR)
    set(USE_EMULATOR 1)
endif()
target_compile_definitions(test_emulator PRIVATE USE_EMULATOR=${USE_EMULATOR})
target_compile_definitions(test_commands PRIVATE USE_EMULATOR=${USE_EMULATOR})

# Include directories
target_include_directories(hostctl PRIVATE ${CMAKE_SOURCE_DIR}/inc ${PROTOCOL_INC} ${ROOT_DIR}/controller/inc ${ROOT_DIR}/os/inc ${PROTOCOL_INC} ${ROOT_DIR}/controller/src)
target_include_directories(test_emulator PRIVATE ${CMAKE_SOURCE_DIR}/inc ${PROTOCOL_INC} ${ROOT_DIR}/controller/inc ${ROOT_DIR}/os/inc ${PROTOCOL_INC} ${ROOT_DIR}/controller/src)
target_include_directories(test_commands PRIVATE ${CMAKE_SOURCE_DIR}/inc ${PROTOCOL_INC} ${ROOT_DIR}/controller/inc ${ROOT_DIR}/os/inc ${PROTOCOL_INC} ${ROOT_DIR}/controller/src)

target_link_libraries(hostctl
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
  PRIVATE protocol-amd64
  PRIVATE Boost::program_options
  PRIVATE Boost::log
  PRIVATE Boost::system
)

target_link_libraries(test_emulator
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
  PRIVATE protocol-amd64
  PRIVATE Boost::program_options
  PRIVATE Boost::log
  PRIVATE Boost::system
)

target_link_libraries(test_commands
  PRIVATE protocol-amd64
  PRIVATE Boost::program_options
  PRIVATE Boost::log
  PRIVATE Boost::system
)

link_compile_commands()

# Host unit tests for the oscilloscope capture bookkeeping (pure logic, no HAL)
add_executable(test_scope_core
    ${ROOT_DIR}/controller/tests/test_scope_capture_core.cc
    ${ROOT_DIR}/controller/src/scope_capture_core.cc
)
target_include_directories(test_scope_core PRIVATE ${ROOT_DIR}/controller/inc)

# Host unit tests for the USB-PD policy helpers (controller/inc/usb_pd_policy.hpp)
add_executable(test_usb_pd_policy ${ROOT_DIR}/controller/tests/test_usb_pd_policy.cc)
target_include_directories(test_usb_pd_policy PRIVATE ${ROOT_DIR}/controller/inc)

# Host unit tests for the link framing (protocol/inc/frame.hpp)
add_executable(test_frame ${ROOT_DIR}/protocol/tests/test_frame.cc)
target_include_directories(test_frame PRIVATE ${ROOT_DIR}/protocol/inc)

# Front panel UI on the host (controller/ui): simulator writing PNG screenshots, unit tests
include(${ROOT_DIR}/controller/ui/sources.cmake)
add_subdirectory(${ROOT_DIR}/external/SSD1327_STM32_Driver ${CMAKE_BINARY_DIR}/ssd1327)
add_library(panel_ui STATIC ${PANEL_UI_SOURCES})
target_include_directories(panel_ui PUBLIC ${PANEL_UI_INC} ${PROTOCOL_INC})
target_link_libraries(panel_ui PUBLIC protocol-amd64)

add_executable(ui_sim src/ui_sim.cc)
target_link_libraries(ui_sim PRIVATE panel_ui ssd1327_core)

add_executable(test_ui ${ROOT_DIR}/controller/ui/tests/test_ui.cc)
target_link_libraries(test_ui PRIVATE panel_ui)
