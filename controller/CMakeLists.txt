nanopb_generate_cpp(TARGET protocol-arm ${PROTOCOL_SOURCES})

add_library(ctl
    src/ctl.cc
    src/interrupts.cc
    src/error.cc
    src/message_handler.cc
    src/memtest.cc
    src/charger.cc
    src/output_voltage_regulator.cc
    src/dac.cc
    src/dac_waveform.cc
    src/dac_controller.cc
)

target_compile_definitions(ctl PRIVATE ${MCU})
target_include_directories(ctl PRIVATE inc ${SFPCONV_INC_DIR} ${TCA9535_INC_DIR} ${STM32_SI5351_INC} ${BOARD_HEADERS_INC} ${OS_API_INC} ${HAL_INCLUDE_DIRS} ${PROTOCOL_INC} ${CMAKE_CURRENT_BINARY_DIR} ${PSRAM_INC_DIR} ${MCP44XX_INC_DIR} ${TPS55289_INC_DIR} ${BQ25792_INC_DIR} ${TCLAP_INCLUDE_DIR})
target_link_libraries(ctl PRIVATE os sfpconv protocol-arm psram_aps6404l mcp44xx tps55289 bq25792)

# Enable exceptions for PSRAM driver integration (aps6404l.hpp uses throw)
target_compile_options(ctl PRIVATE -fexceptions)

if (${CMAKE_BUILD_TYPE} EQUAL "Debug")
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_DEBUG)
else ()
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_INFO)
endif ()
