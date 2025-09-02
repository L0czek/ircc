add_library(ctl
    src/ctl.cc
    src/interrupts.cc
    src/error.cc
)

target_compile_definitions(ctl PRIVATE ${MCU})
target_include_directories(ctl PRIVATE inc ${TCA9535_INC_DIR} ${STM32_SI5351_INC} ${BOARD_HEADERS_INC} ${OS_API_INC} ${HAL_INCLUDE_DIRS})
target_link_libraries(ctl PRIVATE os)

if (${CMAKE_BUILD_TYPE} EQUAL "Debug")
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_DEBUG)
else ()
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_INFO)
endif ()

