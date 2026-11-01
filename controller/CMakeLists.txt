nanopb_generate_cpp(TARGET protocol-arm ${PROTOCOL_SOURCES})

add_library(ctl
    src/ctl.cc
    src/interrupts.cc
    src/error.cc
    src/message_handler.cc
    src/link.cc
    src/usb_cdc_link.cc
    src/broker.cc
    src/scope_service.cc
    src/diagnostics.cc
    src/memtest.cc
    src/i2c_bus.cc
    src/charger.cc
    src/output_voltage_regulator.cc
    src/dac.cc
    src/scope_capture_core.cc
    src/dac_waveform.cc
    src/oscilloscope.cc
    src/board_timer.cc
    src/fan.cc
    src/encoder.cc
    src/ws2812b.cc
    src/board_io.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../applet/src/aux_pwm_applet.cc
)

target_include_directories(ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../applet/inc)

target_compile_definitions(ctl PRIVATE ${MCU})
target_compile_definitions(ctl PRIVATE ${board})

# Reported to the host in DeviceInfo (evaluated when CMake configures)
execute_process(COMMAND git describe --always --dirty --tags
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE FIRMWARE_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT FIRMWARE_GIT_VERSION)
    set(FIRMWARE_GIT_VERSION "unknown")
endif ()
set_source_files_properties(src/diagnostics.cc PROPERTIES COMPILE_DEFINITIONS
    "FIRMWARE_VERSION=\"${FIRMWARE_GIT_VERSION}\";CONTROLLER_BOARD=\"${board}\"")
target_include_directories(ctl PRIVATE inc ${SFPCONV_INC_DIR} ${TCA9535_INC_DIR} ${STM32_SI5351_INC} ${BOARD_HEADERS_INC} ${OS_API_INC} ${HAL_INCLUDE_DIRS} ${PROTOCOL_INC} ${CMAKE_CURRENT_BINARY_DIR} ${PSRAM_INC_DIR} ${MCP44XX_INC_DIR} ${TPS55289_INC_DIR} ${BQ25792_INC_DIR} ${TCLAP_INCLUDE_DIR})
target_include_directories(ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/argum/inc)
target_include_directories(ctl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../external/stubs/include)
target_link_libraries(ctl PRIVATE os sfpconv protocol-arm psram_aps6404l mcp44xx tps55289 bq25792 argum::argum)

# Built without exceptions (toolchain default -fno-exceptions). The drivers report
# errors through std::expected; argum runs in expected-value mode (ARGUM_USE_EXPECTED).

# Ensure argum uses expected values mode (works with or without exceptions)
target_compile_definitions(ctl PRIVATE ARGUM_USE_EXPECTED=1)

# Disable termios for embedded builds (newlib doesn't have sys/termios.h)
target_compile_definitions(ctl PRIVATE ARGUM_NO_TERMIOS=1)

if (${CMAKE_BUILD_TYPE} EQUAL "Debug")
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_DEBUG)
else ()
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_INFO)
endif ()
