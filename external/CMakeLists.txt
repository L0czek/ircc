add_subdirectory(./Si5351-lib)

target_include_directories(Si5351 PRIVATE ${STM32_SI5351_INC} ${HAL_INCLUDE_DIRS})
target_compile_definitions(Si5351 PRIVATE ${MCU})

add_subdirectory(./TCA9535)

target_include_directories(tca9535 PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(tca9535 PRIVATE ${MCU})

add_subdirectory(./sfpconv)

target_include_directories(sfpconv PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(sfpconv PRIVATE ${MCU})

# APS6404L PSRAM Driver - Integrated mode
set(PSRAM_INTEGRATED_MODE ON CACHE BOOL "" FORCE)
set(PSRAM_CMSIS_INC_DIR
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Include
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Device/ST/STM32G4xx/Include
    CACHE STRING "" FORCE)
set(PSRAM_HAL_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/STM32G4xx_HAL_Driver/Inc
    CACHE PATH "" FORCE)
set(PSRAM_MCU ${MCU} CACHE STRING "" FORCE)
add_subdirectory(./APS6404L_STM32_DRIVER)

set(TCA9535_INC_DIR ${TCA9535_INC_DIR} PARENT_SCOPE)
set(STM32_SI5351_INC ${STM32_SI5351_INC} PARENT_SCOPE)
set(SFPCONV_INC_DIR ${SFPCONV_INC_DIR} PARENT_SCOPE)
set(PSRAM_INC_DIR ${PSRAM_INC_DIR} PARENT_SCOPE)
