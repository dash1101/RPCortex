cmake_minimum_required(VERSION 3.13)

# Board defaults to the Pico 2 W (RP2350). Build for an RP2040 with:
#   cmake -B build-rp2040 -DPICO_BOARD=pico_w
# Both are built during the spike, because "does the RP2040 come back" is one of
# the questions the spike exists to answer.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)
project(rpc_loader_spike C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(SPIKE_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(LFS_DIR ${SPIKE_ROOT}/../littlefs)

# --- firmware ---------------------------------------------------------------
add_executable(loader_spike
    firmware/main.cpp
    firmware/loader.cpp
    firmware/api.cpp
    firmware/fault.cpp
    firmware/storage.cpp
    ${LFS_DIR}/lfs.c
    ${LFS_DIR}/lfs_util.c
)

target_include_directories(loader_spike PRIVATE
    ${SPIKE_ROOT}/firmware
    ${SPIKE_ROOT}/include
    ${LFS_DIR}
)

# NOTE: do not define LFS_NO_MALLOC here, not even as 0. lfs.h gates
# lfs_file_open on `#ifndef LFS_NO_MALLOC`, so defining it to 0 removes the
# function rather than enabling it.
target_compile_definitions(loader_spike PRIVATE
    LFS_NO_DEBUG=1
    LFS_NO_WARN=1
    LFS_NO_ERROR=1
    LFS_NO_ASSERT=1
)

target_compile_options(loader_spike PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
)

target_link_libraries(loader_spike
    pico_stdlib
    hardware_flash
    hardware_sync
)

pico_enable_stdio_usb(loader_spike 1)
pico_enable_stdio_uart(loader_spike 0)
pico_add_extra_outputs(loader_spike)

# --- applications -----------------------------------------------------------
# Compiled to RELOCATABLE OBJECTS, never linked. The firmware relocates them at
# load time; that is the whole point of the exercise.
#
# -mlong-calls is deliberately NOT used: it would make the compiler emit
# indirect calls through literal pools for everything, which sidesteps the
# veneer problem by making every call slower. Handling the range in the loader
# keeps app code as fast as firmware code.
set(APP_FLAGS
    -mthumb -Os -std=c++17
    -fno-exceptions -fno-rtti -fno-common -fno-use-cxa-atexit
    -ffunction-sections -fdata-sections
    -I${SPIKE_ROOT}/include
)
if(PICO_PLATFORM STREQUAL "rp2040")
    list(APPEND APP_FLAGS -mcpu=cortex-m0plus)
else()
    list(APPEND APP_FLAGS -mcpu=cortex-m33)
endif()

function(rpc_add_app name)
    set(src ${SPIKE_ROOT}/apps/${name}/${name}.cpp)
    set(out ${CMAKE_BINARY_DIR}/apps/${name}.app)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/apps
        COMMAND ${CMAKE_CXX_COMPILER} ${APP_FLAGS} -c ${src} -o ${out}
        DEPENDS ${src} ${SPIKE_ROOT}/include/rpc_app.h
        COMMENT "Building app ${name}.app (relocatable, not linked)"
        VERBATIM
    )
    add_custom_target(app_${name} ALL DEPENDS ${out})
endfunction()

rpc_add_app(hello)
rpc_add_app(badver)
rpc_add_app(faulty)
