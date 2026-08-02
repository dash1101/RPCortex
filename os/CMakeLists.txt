cmake_minimum_required(VERSION 3.13)

# Per-architecture, not per-board. RP2350A and RP2350B run the SAME ARM binary
# (B just exposes more pins — that is board config, not a build), so one image
# serves both. RP2040 is a separate build; ESP32-S3 is a separate port entirely.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)
project(rpcortex_v2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(OS_ROOT   ${CMAKE_CURRENT_LIST_DIR})
set(SPIKE     ${OS_ROOT}/../loader-spike)
set(LFS_DIR   ${OS_ROOT}/../littlefs)

# The loader, storage glue and fault handler are shared, unchanged, with the
# spike — the spike proved them, the OS uses them. They will move to a shared
# core/ directory once a second consumer needs them changed; until then, one
# copy referenced from both places beats a fork that drifts.
add_executable(rpcortex_v2
    ${OS_ROOT}/main.cpp
    ${OS_ROOT}/api.cpp
    ${OS_ROOT}/platform_rp2.cpp
    ${OS_ROOT}/kernel/kernel.cpp
    ${OS_ROOT}/core/path.cpp
    ${OS_ROOT}/core/pkgindex.cpp
    ${OS_ROOT}/core/textcore.cpp
    ${OS_ROOT}/core/history.cpp
    ${OS_ROOT}/core/sha256.cpp
    ${OS_ROOT}/core/registry.cpp
    ${OS_ROOT}/core/users.cpp
    ${OS_ROOT}/core/persist.cpp
    ${OS_ROOT}/core/out.cpp
    ${OS_ROOT}/core/banner.cpp
    ${OS_ROOT}/shell/command.cpp
    ${OS_ROOT}/shell/session.cpp
    ${OS_ROOT}/shell/fs.cpp
    ${OS_ROOT}/shell/apps.cpp
    ${OS_ROOT}/shell/pkg.cpp
    ${OS_ROOT}/shell/text.cpp
    ${OS_ROOT}/shell/sys.cpp
    ${OS_ROOT}/shell/user.cpp
    ${OS_ROOT}/shell/help.cpp
    ${OS_ROOT}/shell/net.cpp
    ${OS_ROOT}/shell/shell.cpp
    ${SPIKE}/firmware/loader.cpp
    ${SPIKE}/firmware/storage.cpp
    ${SPIKE}/firmware/fault.cpp
    ${LFS_DIR}/lfs.c
    ${LFS_DIR}/lfs_util.c
)

target_include_directories(rpcortex_v2 PRIVATE
    ${OS_ROOT} ${OS_ROOT}/include ${OS_ROOT}/kernel ${OS_ROOT}/shell ${OS_ROOT}/core
    ${SPIKE}/firmware
    ${LFS_DIR}
)

target_compile_definitions(rpcortex_v2 PRIVATE
    LFS_NO_DEBUG=1 LFS_NO_WARN=1 LFS_NO_ERROR=1 LFS_NO_ASSERT=1)

target_compile_options(rpcortex_v2 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

target_link_libraries(rpcortex_v2 pico_stdlib hardware_flash hardware_sync pico_rand pico_aon_timer)

# Wireless is per-BOARD, not per-architecture: the CYW43 part is on the W boards
# only. PICO_CYW43_SUPPORTED comes from the board header, and net.cpp compiles to
# "no radio on this board" stubs without it — so a non-W image still builds, and
# `wifi` still exists to say why it cannot work rather than being missing.
#
# threadsafe_background, not poll: the shell blocks in getchar, so a poll-mode
# stack would never get a turn and DHCP would never complete.
if(PICO_CYW43_SUPPORTED)
    target_link_libraries(rpcortex_v2 pico_cyw43_arch_lwip_threadsafe_background)
endif()
pico_enable_stdio_usb(rpcortex_v2 1)
pico_enable_stdio_uart(rpcortex_v2 0)
pico_add_extra_outputs(rpcortex_v2)

# --- apps (relocatable objects, never linked) ------------------------------
set(APP_FLAGS -mthumb -Os -std=c++17 -fno-exceptions -fno-rtti -fno-common
    -fno-use-cxa-atexit -ffunction-sections -fdata-sections -I${OS_ROOT}/include)
if(PICO_PLATFORM STREQUAL "rp2040")
    list(APPEND APP_FLAGS -mcpu=cortex-m0plus)
else()
    list(APPEND APP_FLAGS -mcpu=cortex-m33)
endif()

function(rpc_add_app name)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/apps/${name}.app
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/apps
        COMMAND ${CMAKE_CXX_COMPILER} ${APP_FLAGS}
                -c ${OS_ROOT}/apps/${name}/${name}.cpp
                -o ${CMAKE_BINARY_DIR}/apps/${name}.app
        DEPENDS ${OS_ROOT}/apps/${name}/${name}.cpp ${OS_ROOT}/include/rpc_app.h
        VERBATIM)
    add_custom_target(app_${name} ALL DEPENDS ${CMAKE_BINARY_DIR}/apps/${name}.app)
endfunction()

rpc_add_app(greet)
