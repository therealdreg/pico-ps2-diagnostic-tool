cmake_minimum_required(VERSION 3.12)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

include(pico_extras_import_optional.cmake)

project(pico_examples C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.5.1")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.5.1 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

set(PICO_EXAMPLES_PATH ${PROJECT_SOURCE_DIR})

set(PICO_DEOPTIMIZED_DEBUG 1)

set(PICO_BOARD pico)

# Initialize the SDK
pico_sdk_init()

include(example_auto_set_url.cmake)

add_compile_options(-Wall
        -Wno-format          # int != int32_t as far as the compiler is concerned because gcc has int32_t as long int
        -Wno-unused-function # we have some for the docs that aren't called
        )
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-maybe-uninitialized)
endif()

add_executable(pico_ps2_diagnostic_tool
pico_ps2_diagnostic_tool.c
        )

# pull in common dependencies
target_link_libraries(pico_ps2_diagnostic_tool
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_flash)

pico_generate_pio_header(pico_ps2_diagnostic_tool ${CMAKE_CURRENT_LIST_DIR}/pico_ps2_diagnostic_tool.pio)

pico_set_binary_type(pico_ps2_diagnostic_tool copy_to_ram)

pico_enable_stdio_uart(pico_ps2_diagnostic_tool 0)
pico_enable_stdio_usb(pico_ps2_diagnostic_tool 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(pico_ps2_diagnostic_tool)

# add url via pico_set_program_url
example_auto_set_url(pico_ps2_diagnostic_tool)

string(APPEND CMAKE_EXE_LINKER_FLAGS "-Wl,--print-memory-usage")

target_compile_definitions(pico_ps2_diagnostic_tool PRIVATE

	)
