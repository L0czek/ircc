add_subdirectory(./Si5351-lib)

target_include_directories(Si5351 PRIVATE ${STM32_SI5351_INC} ${HAL_INCLUDE_DIRS})
target_compile_definitions(Si5351 PRIVATE ${MCU})

add_subdirectory(./TCA9535)

target_include_directories(tca9535 PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(tca9535 PRIVATE ${MCU})

add_subdirectory(./sfpconv)

target_include_directories(sfpconv PRIVATE ${HAL_INCLUDE_DIRS})
target_compile_definitions(sfpconv PRIVATE ${MCU})

# TCLAP - Header-only command line parser
add_subdirectory(./tclap-1.4.0-rc2)

# APS6404L PSRAM Driver - Integrated mode
set(PSRAM_INTEGRATED_MODE ON CACHE BOOL "" FORCE)
set(PSRAM_CMSIS_INC_DIR
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Include
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Device/ST/STM32G4xx/Include
    CACHE STRING "" FORCE)
set(PSRAM_HAL_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/STM32G4xx_HAL_Driver/Inc
    CACHE PATH "" FORCE)
set(PSRAM_MCU ${MCU} CACHE STRING "" FORCE)
set(PSRAM_BOARD_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Core/Inc
    CACHE PATH "" FORCE)
add_subdirectory(./APS6404L_STM32_DRIVER)

# MCP44XX Digital Potentiometer Driver - Integrated mode
set(MCP44XX_INTEGRATED_MODE ON CACHE BOOL "" FORCE)
set(MCP44XX_CMSIS_INC_DIR
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Include
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Device/ST/STM32G4xx/Include
    CACHE STRING "" FORCE)
set(MCP44XX_HAL_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/STM32G4xx_HAL_Driver/Inc
    CACHE PATH "" FORCE)
set(MCP44XX_MCU ${MCU} CACHE STRING "" FORCE)
set(MCP44XX_BOARD_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Core/Inc
    CACHE PATH "" FORCE)
add_subdirectory(./mcp44xx_stm32_driver)

# TPS55289 Buck-Boost Converter Driver - Integrated mode
set(TPS55289_INTEGRATED_MODE ON CACHE BOOL "" FORCE)
set(TPS55289_CMSIS_INC_DIR
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Include
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Device/ST/STM32G4xx/Include
    CACHE STRING "" FORCE)
set(TPS55289_HAL_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/STM32G4xx_HAL_Driver/Inc
    CACHE PATH "" FORCE)
set(TPS55289_MCU ${MCU} CACHE STRING "" FORCE)
set(TPS55289_BOARD_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Core/Inc
    CACHE PATH "" FORCE)
add_subdirectory(./TPS55289_STM32_Driver)

# BQ25792 Charger Driver - Integrated mode
set(BQ25792_INTEGRATED_MODE ON CACHE BOOL "" FORCE)
set(BQ25792_CMSIS_INC_DIR
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Include
    ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/CMSIS/Device/ST/STM32G4xx/Include
    CACHE STRING "" FORCE)
set(BQ25792_HAL_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Drivers/STM32G4xx_HAL_Driver/Inc
    CACHE PATH "" FORCE)
set(BQ25792_MCU ${MCU} CACHE STRING "" FORCE)
set(BQ25792_BOARD_INC_DIR ${CMAKE_SOURCE_DIR}/target/stm32g474re_nucleo_board/Core/Inc
    CACHE PATH "" FORCE)
add_subdirectory(./BQ25792_Driver_STM32)

set(TCA9535_INC_DIR ${TCA9535_INC_DIR} PARENT_SCOPE)
set(STM32_SI5351_INC ${STM32_SI5351_INC} PARENT_SCOPE)
set(SFPCONV_INC_DIR ${SFPCONV_INC_DIR} PARENT_SCOPE)
set(PSRAM_INC_DIR ${PSRAM_INC_DIR} PARENT_SCOPE)
set(MCP44XX_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mcp44xx_stm32_driver/Core/Inc PARENT_SCOPE)
set(TPS55289_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/TPS55289_STM32_Driver/Core/Inc PARENT_SCOPE)
set(BQ25792_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/BQ25792_Driver_STM32/Core/Inc PARENT_SCOPE)
