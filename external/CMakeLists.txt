add_subdirectory(./Si5351-lib)

target_include_directories(Si5351 PRIVATE ${STM32_SI5351_INC} ${HAL_INCLUDE_DIRS})
target_compile_definitions(Si5351 PRIVATE ${MCU})

add_subdirectory(./TCA9535)

target_include_directories(tca9535 PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(tca9535 PRIVATE ${MCU})

add_subdirectory(./sfpconv)

target_include_directories(sfpconv PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(sfpconv PRIVATE ${MCU})

set(TCA9535_INC_DIR ${TCA9535_INC_DIR} PARENT_SCOPE)
set(STM32_SI5351_INC ${STM32_SI5351_INC} PARENT_SCOPE)
set(SFPCONV_INC_DIR ${SFPCONV_INC_DIR} PARENT_SCOPE)
