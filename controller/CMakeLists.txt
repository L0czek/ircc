add_library(ctl
    src/ctl.cc
)

target_compile_definitions(ctl PRIVATE ${MCU})
target_include_directories(ctl PRIVATE inc ${BOARD_HEADERS_INC} ${OS_API_INC} ${HAL_INCLUDE_DIRS})
target_link_libraries(ctl PRIVATE os)

if (${CMAKE_BUILD_TYPE} EQUAL "Debug")
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_DEBUG)
else ()
    target_compile_definitions(ctl PRIVATE LOG_LEVEL_INFO)
endif ()

