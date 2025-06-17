add_library(os
    src/thread.cc
    src/event.cc
    src/mutex.cc
    src/sem.cc
    src/log.cc
)

target_include_directories(os PRIVATE inc ${HAL_INCLUDE_DIRS})
target_compile_definitions(os PRIVATE ${MCU})

set(OS_API_INC "${CMAKE_CURRENT_SOURCE_DIR}/inc" PARENT_SCOPE)
