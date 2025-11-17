nanopb_generate_cpp(TARGET protocol-arm ${PROTOCOL_SOURCES})

add_library(ctl
    src/ctl.cc
    src/interrupts.cc
    src/error.cc
    src/message_handler.cc
)

target_compile_definitions(ctl PRIVATE ${MCU})
target_include_directories(ctl PRIVATE inc ${SFPCONV_INC_DIR} ${TCA9535_INC_DIR} ${STM32_SI5351_INC} ${BOARD_HEADERS_INC} ${OS_API_INC} ${HAL_INCLUDE_DIRS} ${PROTOCOL_INC})
target_link_libraries(ctl PRIVATE os sfpconv protocol-arm)

if (${CMAKE_BUILD_TYPE} EQUAL "Debug")
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_DEBUG)
else ()
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_INFO)
endif ()

