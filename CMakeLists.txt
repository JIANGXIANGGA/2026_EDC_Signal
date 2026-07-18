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

set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "Disable LVGL examples" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "Disable LVGL demos" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "Disable ThorVG" FORCE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "LVGL config path" FORCE)
add_subdirectory(lvgl)

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

add_library(cmsis_dsp_fft STATIC
    Drivers/CMSIS/DSP/Source/CommonTables/arm_common_tables.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_bitreversal2.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_cfft_f32.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_cfft_radix8_f32.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_rfft_fast_init_f32.c
    Drivers/CMSIS/DSP/Source/TransformFunctions/arm_rfft_fast_f32.c
    Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.c
)
target_include_directories(cmsis_dsp_fft PUBLIC
    Drivers/CMSIS/DSP/Include
    Drivers/CMSIS/Include
)
target_compile_definitions(cmsis_dsp_fft PRIVATE
    ARM_DSP_CONFIG_TABLES
    ARM_FFT_ALLOW_TABLES
    ARM_TABLE_TWIDDLECOEF_F32_512
    ARM_TABLE_BITREVIDX_FLT_512
    ARM_TABLE_TWIDDLECOEF_RFFT_F32_1024
    ARM_MATH_LOOPUNROLL
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/dds_app.c
    Modules/DDS/dds.c
    Modules/ADC/adc121s101.c
    Modules/DAC/dac_output.c
    Modules/Display/st7789_bus.c
    Modules/Touch/cst816t.c
    Services/fft_service.c
    Services/signal_service.c
    Services/signal_process.c
    lv_port/lv_port_display.c
    lv_port/lv_port_indev.c
    ui/ui_main.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App
    Modules/DDS
    Modules/ADC
    Modules/DAC
    Modules/Display
    Modules/Touch
    Services
    Drivers/CMSIS/DSP/Include
    lv_port
    ui
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    lvgl
    cmsis_dsp_fft
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
