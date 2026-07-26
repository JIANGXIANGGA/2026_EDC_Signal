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
    Modules/AD9910/ad9910.c
    Modules/DAC/dac_output.c
    Modules/ADC/adc121s101.c
    Services/ad9910_service.c
    Services/ad9910_sweep_planner.c
    Services/adc_dac_loopback_service.c
    App/signal_app.c
    App/ad9910_ram_waveform_app.c
    App/ad9910_ram_waveform_presets.c
    App/ad9910_demo_app.c
    App/ad9910_demo_presets.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Modules/AD9910
    Modules/DAC
    Modules/ADC
    Services/
    App
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
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
