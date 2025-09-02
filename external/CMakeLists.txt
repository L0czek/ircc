set(SI5351_C_FILE ${CMAKE_CURRENT_SOURCE_DIR}/stm32-si5351/si5351/si5351.c)

add_library(stm32_si5351_lib STATIC
    ${SI5351_C_FILE}
)

function(replace_in_file filepath pattern replacement)
    file(READ "${filepath}" file_content)
    string(REPLACE "${pattern}" "${replacement}" new_content "${file_content}")
    file(WRITE "${filepath}" "${new_content}")
endfunction()

replace_in_file(${SI5351_C_FILE} "stm32f1x" "${chip_family}")

set(STM32_SI5351_INC ${CMAKE_CURRENT_SOURCE_DIR}/stm32-si5351/si5351)

target_include_directories(stm32_si5351_lib PRIVATE ${STM32_SI5351_INC} ${HAL_INCLUDE_DIRS})
target_compile_definitions(stm32_si5351_lib PRIVATE ${MCU})

add_subdirectory(./TCA9535)

target_include_directories(tca9535 PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(tca9535 PRIVATE ${MCU})

add_subdirectory(./sfpconv)

target_include_directories(sfpconv PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(sfpconv PRIVATE ${MCU})

set(TCA9535_INC_DIR ${TCA9535_INC_DIR} PARENT_SCOPE)
set(STM32_SI5351_INC ${STM32_SI5351_INC} PARENT_SCOPE)
set(SFPCONV_INC_DIR ${SFPCONV_INC_DIR} PARENT_SCOPE)
