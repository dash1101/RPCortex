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
    ${OS_ROOT}/panic.cpp
    ${OS_ROOT}/task_rp2.cpp
    ${OS_ROOT}/task_switch.S
    ${OS_ROOT}/kernel/kernel.cpp
    ${OS_ROOT}/core/path.cpp
    ${OS_ROOT}/core/httpparse.cpp
    ${OS_ROOT}/core/httpfetch.cpp
    ${OS_ROOT}/core/repoindex.cpp
    ${OS_ROOT}/core/preempt.cpp
    ${OS_ROOT}/core/excframe.cpp
    ${OS_ROOT}/core/tui.cpp
    ${OS_ROOT}/core/tuikey.cpp
    ${OS_ROOT}/core/tuilist.cpp
    ${OS_ROOT}/core/rps.cpp
    ${OS_ROOT}/core/cmdline.cpp
    ${OS_ROOT}/core/interrupt.cpp
    ${OS_ROOT}/core/task.cpp
    ${OS_ROOT}/core/lock.cpp
    ${OS_ROOT}/core/logring.cpp
    ${OS_ROOT}/core/blackbox.cpp
    ${OS_ROOT}/core/joblist.cpp
    ${OS_ROOT}/core/perms.cpp
    ${OS_ROOT}/core/lineedit.cpp
    ${OS_ROOT}/core/fmt.cpp
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
    ${OS_ROOT}/shell/pkgrepo.cpp
    ${OS_ROOT}/shell/diag.cpp
    ${OS_ROOT}/shell/tuiterm.cpp
    ${OS_ROOT}/shell/settings.cpp
    ${OS_ROOT}/shell/editor.cpp
    ${OS_ROOT}/shell/rpscmd.cpp
    ${OS_ROOT}/shell/update.cpp
    ${OS_ROOT}/shell/text.cpp
    ${OS_ROOT}/shell/sys.cpp
    ${OS_ROOT}/shell/user.cpp
    ${OS_ROOT}/shell/help.cpp
    ${OS_ROOT}/shell/net.cpp
    ${OS_ROOT}/shell/netapps.cpp
    ${OS_ROOT}/shell/httptransport.cpp
    ${OS_ROOT}/shell/fetch.cpp
    ${OS_ROOT}/shell/ps.cpp
    ${OS_ROOT}/shell/logcmd.cpp
    ${OS_ROOT}/shell/stock.cpp
    ${OS_ROOT}/shell/jobs.cpp
    ${OS_ROOT}/shell/fsinit.cpp
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
    LFS_NO_DEBUG=1 LFS_NO_WARN=1 LFS_NO_ERROR=1 LFS_NO_ASSERT=1

    # An allocation failure must RETURN NULL, not kill the device. The SDK
    # default (1) panics, which made every "if (!buf)" check in this OS
    # unreachable and turned `meminfo`'s largest-block probe — which asks for
    # allocations it expects to fail — into a crash.
    PICO_MALLOC_PANIC=0

    # The MAIN stack — pid 1, where the shell and therefore every command runs.
    #
    # The SDK default is 2048 bytes. __sbprintf alone needs 1128 of them, and the
    # shell's call chain to a package command is deep: read_line, run_line,
    # run_segment, run_one, apps_launch, app_main, and then whatever the package
    # does. `stress` overflowed it and the device hung — the spawned tasks were
    # given 2-4 KB last pass, but the shell itself was still on the default and
    # is where commands actually execute.
    #
    # 8 KB, with a guard checked at every yield so the next one is reported
    # rather than silently corrupting whatever sits below.
    # The boot stack lives in SCRATCH_Y, which is 4 KB and cannot grow — so this
    # is as large as it goes, and it is why the SHELL is spawned as a proper task
    # with its own allocated stack rather than running on this one. See main.cpp.
    PICO_STACK_SIZE=4096
    PICO_CORE1_STACK_SIZE=4096

    # Route panics through rpc_panic_handler (panic.cpp): print the whole
    # message, let USB drain it, then reboot. The default spins with interrupts
    # off, which truncates the message and wedges the terminal.
    PICO_PANIC_FUNCTION=rpc_panic_handler)

target_compile_options(rpcortex_v2 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>)

# hardware_adc / hardware_i2c: the package ABI exposes both, so the firmware
# has to carry them even when nothing in the OS itself uses a pin.
target_link_libraries(rpcortex_v2 pico_stdlib hardware_flash hardware_sync pico_rand pico_aon_timer pico_multicore pico_flash hardware_adc hardware_i2c hardware_gpio hardware_pio hardware_spi)

# Wireless is per-BOARD, not per-architecture: the CYW43 part is on the W boards
# only. PICO_CYW43_SUPPORTED comes from the board header, and net.cpp compiles to
# "no radio on this board" stubs without it — so a non-W image still builds, and
# `wifi` still exists to say why it cannot work rather than being missing.
#
# threadsafe_background, not poll: the shell blocks in getchar, so a poll-mode
# stack would never get a turn and DHCP would never complete.
# PICO_CYW43_SUPPORTED is a CMake variable, NOT a preprocessor define — the
# board header sets it through pico_board_cmake_set, which only CMake parses.
# Testing it with #if in C++ is therefore always false, which is exactly the bug
# that made a Pico 2 W report "no wireless hardware" while still linking the
# whole driver. The define has to be passed explicitly.
if(PICO_CYW43_SUPPORTED)
    target_link_libraries(rpcortex_v2 pico_cyw43_arch_lwip_threadsafe_background)
    target_compile_definitions(rpcortex_v2 PRIVATE RPC_HAS_WIFI=1)

    # TLS, for the package repo. GitHub serves over HTTPS only, so without this
    # `pkg install <name>` cannot exist — plain HTTP is a testing path, not a
    # shipping one.
    #
    # pico_lwip_mbedtls is the altcp glue; pico_mbedtls is the library plus the
    # SDK's hardware entropy source, which matters more than it looks: a TLS
    # client with a predictable random number generator is not a secure one, and
    # a microcontroller has no other good source.
    target_link_libraries(rpcortex_v2 pico_lwip_mbedtls pico_mbedtls)
    target_compile_definitions(rpcortex_v2 PRIVATE
        RPC_HAS_TLS=1
        MBEDTLS_CONFIG_FILE="${OS_ROOT}/mbedtls_config.h")
endif()
# The trusted roots ride in flash and get written to /os/ca.pem on a first boot,
# the same way the built-in packages do. A file rather than a compiled-in blob,
# so a rotated authority is a download instead of a reflash.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/stock_cacerts.cpp
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
            ${OS_ROOT}/ca_certs.pem ${CMAKE_BINARY_DIR}/stock_cacerts.cpp stock_cacerts
    DEPENDS ${OS_ROOT}/ca_certs.pem ${OS_ROOT}/tools_bin2c.py
)
# The package index rides along too, so `pkg available` works on a device that
# has never been online. v1 shipped its catalogue the same way. It goes stale
# between releases, which is what `pkg update` is for — but a fresh device
# should be able to see what exists before it can reach the network.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/stock_index.cpp
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
            ${OS_ROOT}/../../RPCortex-repo/repo-v2/index.json
            ${CMAKE_BINARY_DIR}/stock_index.cpp stock_index
    DEPENDS ${OS_ROOT}/../../RPCortex-repo/repo-v2/index.json ${OS_ROOT}/tools_bin2c.py
)
add_custom_target(stock_index_src DEPENDS ${CMAKE_BINARY_DIR}/stock_index.cpp)
target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_index.cpp)
add_dependencies(rpcortex_v2 stock_index_src)

add_custom_target(stock_cacerts_src DEPENDS ${CMAKE_BINARY_DIR}/stock_cacerts.cpp)
target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_cacerts.cpp)
add_dependencies(rpcortex_v2 stock_cacerts_src)

# The USB serial buffers, raised from the SDK's 64 bytes.
#
# 64 is fine for typing and far too small for PASTING: a paste arrives as fast
# as USB can deliver it, and anything past 64 bytes was dropped while the screen
# was being redrawn. The symptom was pasted text arriving mangled or not at all,
# which reads as a broken editor rather than a full buffer.
#
# 1 KB in each direction costs 2 KB of RAM against 400 KB free, and covers a
# paste of a whole screenful.
target_compile_definitions(rpcortex_v2 PRIVATE
    CFG_TUD_CDC_RX_BUFSIZE=1024
    CFG_TUD_CDC_TX_BUFSIZE=1024)

pico_enable_stdio_usb(rpcortex_v2 1)
pico_enable_stdio_uart(rpcortex_v2 0)
pico_add_extra_outputs(rpcortex_v2)
# The raw image, which is what an OTA writes to flash.
pico_add_bin_output(rpcortex_v2)

# --- apps (relocatable objects, never linked) ------------------------------
# Packages are built for cortex-m0plus REGARDLESS of the board being built.
#
# ARMv6-M is a strict subset of ARMv8-M Mainline, so an M0+ binary runs on an
# M33 — the reverse is not true. Building per-architecture would mean two
# packages per release in the repository, two things to get wrong at install
# time, and a wrong-arch package that hard-faults rather than failing cleanly.
# One portable binary costs a little speed on the RP2350 and removes that whole
# category. The relocation types GCC emits are identical for both targets
# (R_ARM_ABS32, R_ARM_THM_CALL, R_ARM_THM_JUMP24), which is what makes this safe
# for the loader.
# -fno-jump-tables is not a tuning choice. GCC compiles a large switch into a
# jump table on Thumb-1 and calls a libgcc helper (__gnu_thumb1_case_sqi) to
# index it — a symbol the firmware does not export and should not have to.
# Exporting compiler internals would make the ABI depend on which compiler built
# the package; refusing jump tables keeps a package's dependencies to the fw_*
# surface and nothing else. A branch chain costs a few instructions in code that
# is waiting on a keypress anyway.
#
# Built for cortex-m0plus (ARMv6-M) deliberately: ARMv8-M Baseline is a superset,
# so one binary serves the RP2040 and RP2350 boards alike.
set(APP_FLAGS -mthumb -mcpu=cortex-m0plus -Os -std=c++17
    -fno-exceptions -fno-rtti -fno-common -fno-jump-tables
    -fno-use-cxa-atexit -ffunction-sections -fdata-sections -I${OS_ROOT}/include)

function(rpc_add_app name)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/apps/${name}.app
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/apps
        COMMAND ${CMAKE_CXX_COMPILER} ${APP_FLAGS}
                -c ${OS_ROOT}/apps/${name}/${name}.cpp
                -o ${CMAKE_BINARY_DIR}/apps/${name}.app
        # Check every symbol resolves NOW, not when someone loads it on a device.
        # An unresolved symbol otherwise surfaces as a load failure naming a
        # function the author never wrote.
        COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_checkapp.py
                ${CMAKE_BINARY_DIR}/apps/${name}.app
                ${OS_ROOT}/api.cpp
                ${CMAKE_NM}
        DEPENDS ${OS_ROOT}/apps/${name}/${name}.cpp ${OS_ROOT}/include/rpc_app.h
                ${OS_ROOT}/api.cpp ${OS_ROOT}/tools_checkapp.py
        VERBATIM)
    add_custom_target(app_${name} ALL DEPENDS ${CMAKE_BINARY_DIR}/apps/${name}.app)
endfunction()

rpc_add_app(calc)
rpc_add_app(ws2812)
rpc_add_app(dht)
rpc_add_app(probe)
rpc_add_app(gpio)
rpc_add_app(i2cscan)
rpc_add_app(greet)
rpc_add_app(tuidemo)
rpc_add_app(stress)
rpc_add_app(bench)

# --- packages that ship installed -------------------------------------------
#
# The .app bytes are baked into the firmware and written into /pkg on a first
# boot. That is what keeps them real packages — `pkg remove stress` deletes it
# like any other, and it stays gone — rather than built-in commands wearing a
# package's clothes.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/stock_bench.cpp
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
            ${CMAKE_BINARY_DIR}/apps/bench.app
            ${CMAKE_BINARY_DIR}/stock_bench.cpp stock_bench
    DEPENDS ${CMAKE_BINARY_DIR}/apps/bench.app ${OS_ROOT}/tools_bin2c.py
    VERBATIM)
add_custom_target(stock_bench_src DEPENDS ${CMAKE_BINARY_DIR}/stock_bench.cpp)
target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_bench.cpp)
add_dependencies(rpcortex_v2 stock_bench_src app_bench)

add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/stock_stress.cpp
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
            ${CMAKE_BINARY_DIR}/apps/stress.app
            ${CMAKE_BINARY_DIR}/stock_stress.cpp stock_stress
    DEPENDS ${CMAKE_BINARY_DIR}/apps/stress.app ${OS_ROOT}/tools_bin2c.py
    VERBATIM)
add_custom_target(stock_stress_src DEPENDS ${CMAKE_BINARY_DIR}/stock_stress.cpp)
target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_stress.cpp)
add_dependencies(rpcortex_v2 stock_stress_src app_stress)
