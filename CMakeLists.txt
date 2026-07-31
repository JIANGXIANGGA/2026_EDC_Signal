cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME G474)

# Include toolchain file
include("cmake/gcc-arm-none-eabi.cmake")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME} LANGUAGES C ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

set(SIGNAL_ADC_USE_CUBEMX_GENERATED OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/adc.h" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/adc.c")
    set(SIGNAL_ADC_USE_CUBEMX_GENERATED ON)
endif()

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

set(USER_LINKER_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32g474ve_user.ld")
set(GENERATED_LINKER_SCRIPT_OPTION
    "-T \"${CMAKE_SOURCE_DIR}/stm32g474vetx_flash.ld\"")
string(REPLACE "${GENERATED_LINKER_SCRIPT_OPTION}" ""
       CMAKE_C_LINK_FLAGS "${CMAKE_C_LINK_FLAGS}")
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    "-T${USER_LINKER_SCRIPT}"
)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Modules/ADC/adc_internal.c
    Modules/HMI/tjc_uart_driver.c
    Modules/VOFA/vofa_uart_driver.c
    Services/signal_acquisition_service.c
    Services/signal_measurement_service.c
    Services/usart_hmi_service.c
    Services/vofa_telemetry_service.c
    Services/waveform_analyzer_service.c
    Services/fft.c
    Drivers/CMSIS/DSP/Source/CommonTables/arm_common_tables.c
    Drivers/CMSIS/DSP/Source/CommonTables/arm_const_structs.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_bitreversal2.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_cfft_f32.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_cfft_radix8_f32.c
    App/signal_app.c
    App/signal_hmi_app.c
)

if(NOT SIGNAL_ADC_USE_CUBEMX_GENERATED)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc.c
        Drivers/STM32G4xx_HAL_Driver/Src/stm32g4xx_hal_adc_ex.c
    )
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Modules/ADC
    Modules/HMI
    Modules/VOFA
    Services/
    App
    Drivers/CMSIS/DSP/Include
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    ARM_MATH_CM4
    SIGNAL_ADC_USE_CUBEMX_GENERATED=$<BOOL:${SIGNAL_ADC_USE_CUBEMX_GENERATED}>
    ARM_MATH_LOOPUNROLL
    ARM_DSP_CONFIG_TABLES
    ARM_FFT_ALLOW_TABLES
    ARM_TABLE_TWIDDLECOEF_F32_4096
    ARM_TABLE_BITREVIDX_FLT_4096
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    m
)

get_filename_component(ARM_TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(ARM_OBJCOPY_EXECUTABLE
    NAMES arm-none-eabi-objcopy
    HINTS "${ARM_TOOLCHAIN_BIN_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND "${ARM_OBJCOPY_EXECUTABLE}" -O ihex
            "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.hex"
    COMMAND "${ARM_OBJCOPY_EXECUTABLE}" -O binary
            "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>"
            "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>/${CMAKE_PROJECT_NAME}.bin"
    COMMENT "Generating HEX and BIN firmware images"
    VERBATIM
)
