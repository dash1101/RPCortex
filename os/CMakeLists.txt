cmake_minimum_required(VERSION 3.13)

# Per-architecture, not per-board. RP2350A and RP2350B run the SAME ARM binary
# (B just exposes more pins — that is board config, not a build), so one image
# serves both. RP2040 is a separate build; ESP32-S3 is a separate port entirely.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

# SIZE, NOT SPEED. The SDK's default is Release, which is -O3 — and every trick
# -O3 has for going faster buys the speed with bytes: inlining a function into
# each of its callers, unrolling a loop so its body appears several times,
# duplicating a path so each copy can be specialised.
#
# On this family flash is the binding constraint. A Pico W holds 2 MB, and at
# -O3 the image was 1002 KB against a 1024 KB slot, which left nothing for a
# filesystem — a whole board unusable to buy speed nobody had measured.
# MinSizeRel is -Os, and -Os is -O2 with the size-expensive tricks turned off
# rather than a different, worse compiler. It costs 214 KB less:
#
#     pico_w  -O3  1002 KB      pico_w  -Os  788 KB
#
# On RP2040 it may also be FASTER. Code executes from flash through a small XIP
# cache, so a smaller image is a better-cached one, and a cache hit beats an
# unrolled loop that has to be fetched. `probe` measures busy_wait accuracy and
# jitter, which is where any regression would show.
#
# WHICH PART, THOUGH. The trade is not the same on both.
#
# RP2040 holds 2 MB and has no room to spare, so it takes -Os and the 214 KB
# that buys. RP2350 holds 4 MB: at -O3 the image is 955 KB, which still leaves
# 2 MB of filesystem and 68 KB of slot headroom, so it pays nothing at all for
# the speed.
#
# And the speed is not theoretical. `probe` times 20,000 supervisor calls, and
# that is the one number a package feels on every ABI call it makes:
#
#     -O3   296 cycles        -Os   405 cycles
#
# 37% on the path Nova D1 will live in, for size a 4 MB board does not need.
# The rest of the workload barely noticed — RPCMark came out slightly HIGHER at
# -Os — so this is specifically about the supervisor call, not about speed in
# general.
#
# Overridable either way: -DCMAKE_BUILD_TYPE= on the command line wins, and
# ./build.sh --fast forces -O3 everywhere for comparing.
if(NOT CMAKE_BUILD_TYPE)
    if(PICO_BOARD STREQUAL "pico" OR PICO_BOARD STREQUAL "pico_w")
        set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)   # -Os: 2 MB of flash
    else()
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)      # -O3: 4 MB, spend it
    endif()
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
    ${OS_ROOT}/usb_descriptors.c
    ${OS_ROOT}/usbdev.cpp
    ${OS_ROOT}/usbmsc.cpp
    ${OS_ROOT}/usbhid.cpp
    ${OS_ROOT}/core/ducky.cpp
    ${OS_ROOT}/core/fatview.cpp
    ${OS_ROOT}/core/fat12.cpp
    ${OS_ROOT}/core/blockcache.cpp
    ${OS_ROOT}/core/ptrcheck.cpp
    ${OS_ROOT}/platform_rp2.cpp
    ${OS_ROOT}/panic.cpp
    ${OS_ROOT}/task_rp2.cpp
    ${OS_ROOT}/task_switch.S
    ${OS_ROOT}/mpu_rp2.cpp
    ${OS_ROOT}/sandbox_rp2.cpp
    ${OS_ROOT}/sandbox_switch.S
    ${OS_ROOT}/core/mpu.cpp
    ${OS_ROOT}/core/arena.cpp
    ${OS_ROOT}/kernel/kernel.cpp
    ${OS_ROOT}/core/path.cpp
    ${OS_ROOT}/core/repoindex.cpp
    ${OS_ROOT}/core/preempt.cpp
    ${OS_ROOT}/core/excframe.cpp
    ${OS_ROOT}/core/tui.cpp
    ${OS_ROOT}/core/tuikey.cpp
    ${OS_ROOT}/core/tuilist.cpp
    ${OS_ROOT}/core/rps.cpp
    ${OS_ROOT}/core/cmdline.cpp
    ${OS_ROOT}/core/interrupt.cpp
    ${OS_ROOT}/shell/power.cpp
    ${OS_ROOT}/core/stagesink.cpp
    ${OS_ROOT}/core/svcpolicy.cpp
    ${OS_ROOT}/core/powerpolicy.cpp
    ${OS_ROOT}/core/framebuf.cpp
    ${OS_ROOT}/core/task.cpp
    ${OS_ROOT}/core/lock.cpp
    ${OS_ROOT}/core/logring.cpp
    ${OS_ROOT}/core/crashlog.cpp
    ${OS_ROOT}/core/idle.cpp
    ${OS_ROOT}/core/clockcfg.cpp
    ${OS_ROOT}/core/secret.cpp
    ${OS_ROOT}/core/protect.cpp
    ${OS_ROOT}/core/blackbox.cpp
    ${OS_ROOT}/core/joblist.cpp
    ${OS_ROOT}/core/detach.cpp
    ${OS_ROOT}/core/perms.cpp
    ${OS_ROOT}/core/sudo.cpp
    ${OS_ROOT}/core/lineedit.cpp
    ${OS_ROOT}/core/fmt.cpp
    ${OS_ROOT}/core/pkgindex.cpp
    ${OS_ROOT}/core/textcore.cpp
    ${OS_ROOT}/core/history.cpp
    ${OS_ROOT}/core/sha256.cpp
    ${OS_ROOT}/core/imagecheck.cpp
    ${OS_ROOT}/core/b64.cpp
    ${OS_ROOT}/core/linkreq.cpp
    ${OS_ROOT}/core/registry.cpp
    ${OS_ROOT}/core/users.cpp
    ${OS_ROOT}/core/ident.cpp
    ${OS_ROOT}/core/persist.cpp
    ${OS_ROOT}/core/out.cpp
    ${OS_ROOT}/core/banner.cpp
    ${OS_ROOT}/shell/command.cpp
    ${OS_ROOT}/shell/session.cpp
    ${OS_ROOT}/shell/fs.cpp
    ${OS_ROOT}/shell/link.cpp
    ${OS_ROOT}/shell/apps.cpp
    ${OS_ROOT}/shell/pkg.cpp
    ${OS_ROOT}/shell/pkgrepo.cpp
    ${OS_ROOT}/shell/diag.cpp
    ${OS_ROOT}/shell/tuiterm.cpp
    ${OS_ROOT}/shell/settings.cpp
    ${OS_ROOT}/shell/editor.cpp
    ${OS_ROOT}/shell/rpscmd.cpp
    ${OS_ROOT}/shell/update.cpp
    ${OS_ROOT}/shell/rollback.cpp
    ${OS_ROOT}/shell/text.cpp
    ${OS_ROOT}/shell/sys.cpp
    ${OS_ROOT}/shell/user.cpp
    ${OS_ROOT}/shell/help.cpp
    ${OS_ROOT}/shell/imagefeatures.cpp
    ${OS_ROOT}/shell/fetch.cpp
    # The HTTP client and everything on top of it, in every image: without Wi-Fi
    # a request goes through the bridge (shell/bridge.cpp) instead of lwIP.
    ${OS_ROOT}/shell/httpclient.cpp
    ${OS_ROOT}/shell/pkgfetch.cpp
    ${OS_ROOT}/shell/update_net.cpp
    ${OS_ROOT}/core/httpfetch.cpp
    ${OS_ROOT}/core/httpparse.cpp
    ${OS_ROOT}/shell/ps.cpp
    ${OS_ROOT}/shell/logcmd.cpp
    ${OS_ROOT}/shell/devkey.cpp
    ${OS_ROOT}/shell/fsguard.cpp
    ${OS_ROOT}/shell/usbdrive.cpp
    ${OS_ROOT}/shell/hid.cpp
    ${OS_ROOT}/shell/stock.cpp
    ${OS_ROOT}/shell/jobs.cpp
    ${OS_ROOT}/shell/fsinit.cpp
    ${OS_ROOT}/shell/shell.cpp
    ${SPIKE}/firmware/loader.cpp
    ${SPIKE}/firmware/pkgslot.cpp
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

# --- features: what this image is built with ------------------------------------
#
# An image can be built with or without the optional parts: Wi-Fi, Bluetooth,
# SSH, the microSD driver, the add-on drivers and the development packages.
# Each one is whole files in or out of the build (and its libraries linked or
# not), never a command switched off at run time. A feature that is out is
# represented by a small stand-in file (net_off.cpp, bt_off.cpp, ssh_off.cpp,
# periph_off.cpp, sd_off.cpp, radio_off.cpp) that keeps its commands and package
# calls in place and has them say why they cannot work.
#
# THE TABLE IS tools/features.py, not this file: which boards can have each
# feature, what is on by default, and what needs what. build.sh, the
# interactive tools/buildconf.py and tools/release.py read the same table, so
# they cannot disagree with the build. This asks it once per configure, passing
# on whatever was given:
#
#     -DRPC_FEATURE_WIFI=OFF  -DRPC_FEATURE_BLUETOOTH=OFF  -DRPC_SSH=OFF
#     -DRPC_FEATURE_SD=OFF    -DRPC_FEATURE_PERIPHERALS=OFF  -DRPC_DEV_PACKAGES=ON
#
# Anything not given takes the board's standard setting: every feature the
# board can have, except SSH on the Pico W (too tight a slot) and the
# development packages. Asking for a feature the board cannot have stops the
# configure. build.sh always passes every one of
# them, because a build directory remembers what it was configured with, and a
# setting left over from an earlier configure is how a -Os default once went
# unnoticed for a whole rebuild.
#
# The flash layout below (RPC_FW_RESERVE, the package slot) does NOT depend on
# the features. A custom image saves slot headroom and RAM, never filesystem,
# so moving a board between a standard and a custom image keeps its files.
set(RPC_FEATURE_OPTIONS
    RPC_FEATURE_WIFI RPC_FEATURE_BLUETOOTH RPC_SSH RPC_FEATURE_SD
    RPC_FEATURE_PERIPHERALS RPC_DEV_PACKAGES)
set(_rpc_given "")
foreach(_opt ${RPC_FEATURE_OPTIONS})
    if(DEFINED ${_opt})
        list(APPEND _rpc_given "${_opt}=${${_opt}}")
    endif()
endforeach()
execute_process(
    COMMAND python3 ${OS_ROOT}/../tools/features.py cmake ${PICO_BOARD}
            ${CMAKE_BINARY_DIR}/features.cmake ${_rpc_given}
    RESULT_VARIABLE _rpc_rc ERROR_VARIABLE _rpc_err)
if(NOT _rpc_rc EQUAL 0)
    message(FATAL_ERROR "Feature selection for ${PICO_BOARD}: ${_rpc_err}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OS_ROOT}/../tools/features.py)
include(${CMAKE_BINARY_DIR}/features.cmake)
if(RPC_FEATURE_NOTES)
    message(STATUS "Features: ${RPC_FEATURE_NOTES}")
endif()

# --- microSD: on the boards that have room for it ------------------------------
#
# The SD driver is an SPI block layer, a FAT16/32 reader and a `sd` command, and
# together they are a few kilobytes of flash. A few kilobytes is nothing on an
# RP2350 and it is a meaningful slice of what a Pico W has left. Slot headroom at
# build 413, release build (build.sh prints the live figure every build):
#
#   pico2_w    39 KB headroom      pico2   131 KB headroom
#   pico_w     46 KB headroom      pico    104 KB headroom
#
# pico2_w is the tight board now, not pico_w: it carries the same radio blob AND
# the package slot the RP2040 boards do not have. Older notes calling pico_w the
# constraint predate that slot.
#
# The reason it is off on the RP2040 boards is not only the budget, though. Nova
# D1 — the thing with a card slot wired to it — targets the RP2350 boards, and
# novaboard's own detect() refuses to claim an RP2040 board for exactly the same
# reason: the header matches and the part cannot carry the software. A Pico W
# with this compiled in would carry a driver for a slot it does not have. So
# tools/features.py offers `sd` on the RP2350 boards only, where it is standard
# and can be left out.
#
# THE DEFINE HAS TO BE A DEFINE. PICO_CYW43_SUPPORTED is a CMake variable and
# testing it with #if was silently always false for a whole release — the note
# further down this file is about that exact bug. So RPC_HAS_SD is passed as a
# compile definition (with the other features, at the end of this file), and
# `nm` on the two images is what proves it landed: a pico2_w image has sd_mount
# in it and a pico_w image does not.
message(STATUS "microSD support: ${RPC_FEATURE_SD}")

if(RPC_FEATURE_SD)
    set(RPC_SD_SOURCES
        ${OS_ROOT}/core/sdproto.cpp
        ${OS_ROOT}/core/fatro.cpp
        ${OS_ROOT}/sdcard_rp2.cpp
        ${OS_ROOT}/shell/sd.cpp)
    target_sources(rpcortex_v2 PRIVATE ${RPC_SD_SOURCES})

    # -Os for these four, against the -O3 the rest of an RP2350 image gets.
    #
    # The argument at the top of this file is that RP2350 has flash to spare and
    # the supervisor call is worth 37% of its speed. Neither half applies here.
    # Every one of these functions is waiting on a 12 MHz SPI bus — a block read
    # is 400 microseconds of clocking whatever the code around it was compiled
    # at — and the flash is not free after all: pico2_w has 85 KB of slot
    # headroom, and at -O3 this cost 16 KB of it.
    set_source_files_properties(${RPC_SD_SOURCES} PROPERTIES COMPILE_OPTIONS -Os)
else()
    # `sd` says the driver was left out, on a board that could have had it.
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/sd_off.cpp)
endif()

# --- add-on drivers -----------------------------------------------------------
#
# NFC (PN532 over I2C), iButton (DS1990A over 1-Wire), sub-GHz (CC1101) and LoRa
# (SX1276) over SPI: the drivers the OS itself carries for add-on boards, with
# their commands and the fw_nfc_* package calls. Nothing else in the OS calls
# into them, so they come out as a block. Without them periph_off.cpp keeps the
# commands and the NFC calls, which answer as a reader that is not there.
set(RPC_PERIPHERALS_SOURCES
    ${OS_ROOT}/shell/nfc.cpp
    ${OS_ROOT}/shell/ibutton.cpp
    ${OS_ROOT}/shell/cc1101.cpp
    ${OS_ROOT}/shell/sx1276.cpp
    ${OS_ROOT}/core/nfcframe.cpp
    ${OS_ROOT}/core/onewire.cpp)
if(RPC_FEATURE_PERIPHERALS)
    target_sources(rpcortex_v2 PRIVATE ${RPC_PERIPHERALS_SOURCES})
else()
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/periph_off.cpp)
endif()

# COLD CODE AT -Os, HOT CODE AT -O3.
#
# An RP2350 image defaults to -O3 (see the top of this file), and that is worth it
# for the code that runs on every package call: the scheduler, the sandbox and the
# ABI. It is not worth it for the rest, which is waiting on a serial line, a radio
# or a flash write whatever it was compiled at. Measured on a pico2_w: the whole
# image at -Os is 797 KB against 1009 KB, and where it comes from is the OS shell
# (84 KB), the Bluetooth stack (51 KB), TLS (26 KB), littlefs (12 KB) and lwIP
# (11 KB). None of those is on the supervisor-call path.
#
# So those get -Os and everything else keeps -O3. Off with -DRPC_COLD_SIZE=OFF to
# compare. RP2040 boards already build everything at -Os, so this changes nothing
# there.
option(RPC_COLD_SIZE "Build cold code at -Os on -O3 images" ON)
if(RPC_COLD_SIZE AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # The shell and the libraries, wholesale.
    file(GLOB RPC_COLD_SHELL ${OS_ROOT}/shell/*.cpp)
    file(GLOB_RECURSE RPC_COLD_LIBS
        ${PICO_SDK_PATH}/lib/btstack/*.c
        ${PICO_SDK_PATH}/lib/lwip/src/*.c
        ${PICO_SDK_PATH}/lib/mbedtls/library/*.c
        ${PICO_SDK_PATH}/lib/cyw43-driver/src/*.c
        ${PICO_SDK_PATH}/src/rp2_common/pico_btstack/*.c
        ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/*.c
        ${PICO_SDK_PATH}/src/rp2_common/pico_lwip/*.c)
    # And the parts of core/ that are bound by a USB host, a flash write or a
    # person typing, one by one, because core/ also holds the scheduler and the
    # locks, which stay at -O3. Each of these is named so that adding a file to
    # core/ does not move it into this list by accident.
    set(RPC_COLD_CORE
        ${OS_ROOT}/core/fat12.cpp ${OS_ROOT}/core/fatview.cpp ${OS_ROOT}/core/fatro.cpp
        ${OS_ROOT}/core/rps.cpp ${OS_ROOT}/core/repoindex.cpp ${OS_ROOT}/core/pkgindex.cpp
        ${OS_ROOT}/core/ducky.cpp ${OS_ROOT}/core/users.cpp
        ${OS_ROOT}/core/sudo.cpp ${OS_ROOT}/core/perms.cpp
        ${OS_ROOT}/core/httpfetch.cpp ${OS_ROOT}/core/httpparse.cpp ${OS_ROOT}/core/bridge.cpp
        ${OS_ROOT}/usbmsc.cpp ${OS_ROOT}/usbhid.cpp)
    set_source_files_properties(
        ${RPC_COLD_SHELL} ${RPC_COLD_LIBS} ${RPC_COLD_CORE}
        ${LFS_DIR}/lfs.c ${LFS_DIR}/lfs_util.c
        PROPERTIES COMPILE_OPTIONS -Os)
endif()

# WHICH BUILD THIS IS, baked in.
#
# `ver` used to print a registry key that defaults to "1" and that nothing ever
# writes, so every image ever produced claimed to be build 1. Eight different
# firmwares went onto one board in a single night and none of them could be told
# from another — which makes "what is this device running" unanswerable at
# exactly the moment somebody needs to know.
#
# The short commit, plus a mark when the tree was dirty, because an image built
# from uncommitted work is not the commit it claims and saying so is the whole
# point. Falls back to "nogit" rather than failing the build: a tarball with no
# repository in it should still compile.
execute_process(COMMAND git -C ${OS_ROOT}/.. rev-parse --short=8 HEAD
                OUTPUT_VARIABLE RPC_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE RPC_GIT_RC)
if(NOT RPC_GIT_RC EQUAL 0)
    set(RPC_GIT_SHA "nogit")
else()
    execute_process(COMMAND git -C ${OS_ROOT}/.. status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE RPC_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT RPC_GIT_DIRTY STREQUAL "")
        set(RPC_GIT_SHA "${RPC_GIT_SHA}+")
    endif()
endif()
message(STATUS "build id: ${RPC_GIT_SHA}")

# The build NUMBER, which is the commit count. A beta's version is frozen (see
# kernel.h), so this is what actually distinguishes two images and what the
# updater compares — and deriving it means it cannot be forgotten. Unlike the
# sha it never carries a '+': it has to stay a number to compare.
execute_process(COMMAND git -C ${OS_ROOT}/.. rev-list --count HEAD
                OUTPUT_VARIABLE RPC_GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE RPC_COUNT_RC)
if(NOT RPC_COUNT_RC EQUAL 0 OR RPC_GIT_COUNT STREQUAL "")
    set(RPC_GIT_COUNT "0")
endif()
message(STATUS "build number: ${RPC_GIT_COUNT}")

target_compile_definitions(rpcortex_v2 PRIVATE
    RPC_BUILD_ID="${RPC_GIT_SHA}"
    RPC_OS_BUILD="${RPC_GIT_COUNT}"

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
target_link_libraries(rpcortex_v2 pico_stdlib hardware_flash hardware_sync pico_rand pico_aon_timer pico_multicore pico_flash hardware_adc hardware_i2c hardware_gpio hardware_pio hardware_spi hardware_pwm hardware_uart pico_low_power)

# --- the radios: Wi-Fi and Bluetooth, on the W boards ------------------------
#
# Wireless is per-BOARD, not per-architecture: the CYW43 part is on the W boards
# only. Which of its two radios an image carries is a feature (see the features
# section above), and each is whole files in or out:
#
#   Wi-Fi      RPC_WIFI_SOURCES, lwIP, mbedTLS and the trusted roots, the
#              CYW43 driver and its firmware, and radio.cpp (the chip and the
#              one-owner rule both radios share)
#   Bluetooth  RPC_BLUETOOTH_SOURCES and btstack, on top of Wi-Fi
#   no Wi-Fi   radio_off.cpp, and no CYW43 driver or firmware at all; the
#              network bridge (bridge.cpp) in place of lwIP, so the HTTP
#              client, which every image has, goes through the host instead
#
# Bluetooth needs Wi-Fi (tools/features.py): the chip's driver and its
# firmware come in with Wi-Fi, so an image without Wi-Fi has no radio at all.
#
# Without Wi-Fi, net_off.cpp stands in for the network files and without
# Bluetooth bt_off.cpp does, so the commands still exist and say why they
# cannot run. The plain boards are simply boards on which neither can be on.
#
# threadsafe_background, not poll: the shell blocks in getchar, so a poll-mode
# stack would never get a turn and DHCP would never complete.
#
# PICO_CYW43_SUPPORTED is a CMake variable, NOT a preprocessor define — the
# board header sets it through pico_board_cmake_set, which only CMake parses.
# Testing it with #if in C++ is therefore always false, which is exactly the bug
# that made a Pico 2 W report "no wireless hardware" while still linking the
# whole driver. So every define the source tests is passed explicitly, and the
# board fact (RPC_HAS_CYW43: GPIO 23/24/25/29 belong to the chip) is passed
# separately from the features, because it holds whatever the image carries.
set(RPC_WIFI_SOURCES
    ${OS_ROOT}/shell/net.cpp
    ${OS_ROOT}/shell/netapps.cpp
    ${OS_ROOT}/shell/nettcp.cpp
    ${OS_ROOT}/shell/httptransport.cpp
    ${OS_ROOT}/core/scanlist.cpp
    ${OS_ROOT}/core/joinguard.cpp)
set(RPC_BLUETOOTH_SOURCES
    ${OS_ROOT}/shell/bt.cpp
    ${OS_ROOT}/shell/btaudio.cpp
    ${OS_ROOT}/core/btname.cpp
    ${OS_ROOT}/core/pcmring.cpp
    ${OS_ROOT}/core/wav.cpp)

if(PICO_CYW43_SUPPORTED)
    target_compile_definitions(rpcortex_v2 PRIVATE RPC_HAS_CYW43=1)
else()
    target_compile_definitions(rpcortex_v2 PRIVATE RPC_HAS_CYW43=0)
endif()

if(RPC_FEATURE_WIFI)
    target_sources(rpcortex_v2 PRIVATE ${RPC_WIFI_SOURCES})
    target_link_libraries(rpcortex_v2 pico_cyw43_arch_lwip_threadsafe_background)

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

    # The trusted roots ride in flash and get written to /os/ca.pem on a first
    # boot, the same way the built-in packages do. A file rather than a
    # compiled-in blob, so a rotated authority is a download instead of a reflash.
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/stock_cacerts.cpp
        COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
                ${OS_ROOT}/ca_certs.pem ${CMAKE_BINARY_DIR}/stock_cacerts.cpp stock_cacerts
        DEPENDS ${OS_ROOT}/ca_certs.pem ${OS_ROOT}/tools_bin2c.py
    )
    add_custom_target(stock_cacerts_src DEPENDS ${CMAKE_BINARY_DIR}/stock_cacerts.cpp)
    target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_cacerts.cpp)
    add_dependencies(rpcortex_v2 stock_cacerts_src)
else()
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/net_off.cpp
        ${OS_ROOT}/shell/bridge.cpp ${OS_ROOT}/core/bridge.cpp)
endif()

if(RPC_FEATURE_BLUETOOTH)
    # Bluetooth. Both radios are the same chip, which is what makes this
    # possible at all — and Classic is the capability MicroPython could not
    # reach, so a media player is a v2 thing and was never a v1 one.
    target_sources(rpcortex_v2 PRIVATE ${RPC_BLUETOOTH_SOURCES})
    target_link_libraries(rpcortex_v2
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        pico_btstack_run_loop_async_context
        pico_btstack_sbc_encoder)
else()
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/bt_off.cpp)
endif()

if(RPC_FEATURE_WIFI)
    # The chip and the one-owner rule, shared by Wi-Fi and Bluetooth.
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/radio.cpp)
else()
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/radio_off.cpp)
endif()

# --- the radio firmware, stored deflated --------------------------------------
#
# The CYW43 firmware the SDK embeds (the Wi-Fi firmware and its CLM, 227 KB as
# one array) was the largest single object in a wireless image, stored exactly
# as it is sent to the chip. Deflated it is 148 KB, which is 78 KB of slot back
# on both W boards, and on a Pico W that is the difference between room for the
# next feature and none.
#
# Three pieces:
#   tools_cyw43fw.py   deflates the SDK's array at build time, with a 2 KB window
#                      and a CRC-32 per window (cyw43_fw_data.c, not in git);
#   core/fwstream.cpp  inflates it a window at a time and checks each window
#                      before handing out any of it (uzlib, in uzlib/);
#   patches/cyw43-fw-stream.patch
#                      makes the driver take the firmware from that reader. It
#                      is applied to a COPY of cyw43_ll.c in this build
#                      directory, never to the SDK: sdk/ is cloned, often shared
#                      between checkouts, and not this project's to edit. A
#                      fresh copy is patched at every configure, so applying it
#                      is idempotent by construction, and a patch that no longer
#                      applies stops the configure rather than building the
#                      driver as it was.
#
# Off with -DRPC_CYW43_FW_STREAM=OFF, which builds the SDK's driver untouched.
#
# Only where the driver is in the image at all, which is when Wi-Fi is (the
# features section): an image built without Wi-Fi has no CYW43 firmware to
# store, and neither the patch step nor cyw43_fw.cpp is in it.
option(RPC_CYW43_FW_STREAM "Store the CYW43 firmware deflated" ON)
if(PICO_CYW43_SUPPORTED AND RPC_CYW43_FW_STREAM AND RPC_FEATURE_WIFI)
    set(CYW43_FW_DIR   ${CMAKE_BINARY_DIR}/cyw43_fw)
    set(CYW43_LL_SDK   ${PICO_CYW43_DRIVER_PATH}/src/cyw43_ll.c)
    set(CYW43_FW_PATCH ${OS_ROOT}/patches/cyw43-fw-stream.patch)
    # The firmware the SDK picks: the Bluetooth-capable one when Bluetooth is
    # linked (every standard wireless image), the plain one in an image built
    # without Bluetooth. The two are the same layout, the Wi-Fi firmware and
    # then the CLM. cyw43_fw.h checks that the stream matches the driver's
    # choice (RPC_CYW43_FW_BT). DEVICE-UNCONFIRMED: an image streaming the
    # plain firmware has not run on a board.
    if(RPC_FEATURE_BLUETOOTH)
        set(CYW43_FW_BLOB  ${PICO_CYW43_DRIVER_PATH}/firmware/wb43439A0_7_95_49_00_combined.h)
        set(RPC_CYW43_FW_BT 1)
    else()
        set(CYW43_FW_BLOB  ${PICO_CYW43_DRIVER_PATH}/firmware/w43439A0_7_95_49_00_combined.h)
        set(RPC_CYW43_FW_BT 0)
    endif()

    find_program(RPC_PATCH_TOOL patch)
    if(NOT RPC_PATCH_TOOL)
        message(FATAL_ERROR "The 'patch' program is needed to build the radio driver "
            "(os/patches/cyw43-fw-stream.patch). Install it, or configure with "
            "-DRPC_CYW43_FW_STREAM=OFF to build with the firmware stored as it is.")
    endif()
    file(REMOVE_RECURSE ${CYW43_FW_DIR}/work)
    configure_file(${CYW43_LL_SDK} ${CYW43_FW_DIR}/work/src/cyw43_ll.c COPYONLY)
    execute_process(
        COMMAND ${RPC_PATCH_TOOL} -p1 -N -t -F 0 -i ${CYW43_FW_PATCH}
        WORKING_DIRECTORY ${CYW43_FW_DIR}/work
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "os/patches/cyw43-fw-stream.patch does not apply to "
            "${CYW43_LL_SDK}:\n${out}\nThe SDK's cyw43-driver has probably changed. "
            "Bring the patch up to date, or configure with -DRPC_CYW43_FW_STREAM=OFF "
            "to build with the firmware stored as it is.")
    endif()
    # Copied on only when it changed, so a configure does not rebuild the driver.
    configure_file(${CYW43_FW_DIR}/work/src/cyw43_ll.c ${CYW43_FW_DIR}/cyw43_ll.c COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CYW43_FW_PATCH})

    # Swap the patched copy in for the SDK's file in the driver's own source
    # list, and say so if the SDK stopped listing it there: a swap that silently
    # did nothing would link the stock driver against no firmware at all.
    get_target_property(_cyw43_srcs cyw43_driver INTERFACE_SOURCES)
    list(FIND _cyw43_srcs ${CYW43_LL_SDK} _cyw43_ll_at)
    if(_cyw43_ll_at EQUAL -1)
        message(FATAL_ERROR "cyw43_driver no longer builds ${CYW43_LL_SDK}; "
            "os/CMakeLists.txt cannot swap in the patched copy.")
    endif()
    list(REMOVE_AT _cyw43_srcs ${_cyw43_ll_at})
    list(INSERT _cyw43_srcs ${_cyw43_ll_at} ${CYW43_FW_DIR}/cyw43_ll.c)
    set_property(TARGET cyw43_driver PROPERTY INTERFACE_SOURCES ${_cyw43_srcs})

    # --pad 4 is CYW43_WRITE_BYTES_PAD on the SPI bus; the patched driver
    # static_asserts that the stream is exactly the length it sends.
    add_custom_command(
        OUTPUT ${CYW43_FW_DIR}/cyw43_fw_data.c ${CYW43_FW_DIR}/cyw43_fw_data.h
        COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_cyw43fw.py --pad 4
                ${CYW43_FW_BLOB} ${CYW43_FW_DIR}/cyw43_fw_data
        DEPENDS ${CYW43_FW_BLOB} ${OS_ROOT}/tools_cyw43fw.py)
    set(RPC_FW_STREAM_SOURCES
        ${OS_ROOT}/cyw43_fw.cpp ${OS_ROOT}/core/fwstream.cpp
        ${OS_ROOT}/uzlib/tinflate.c ${OS_ROOT}/uzlib/crc32.c ${OS_ROOT}/uzlib/adler32.c)
    target_sources(rpcortex_v2 PRIVATE ${RPC_FW_STREAM_SOURCES}
        ${CYW43_FW_DIR}/cyw43_fw_data.c ${CYW43_FW_DIR}/cyw43_fw_data.h)
    target_include_directories(rpcortex_v2 PRIVATE ${CYW43_FW_DIR} ${OS_ROOT}/uzlib)
    target_compile_definitions(rpcortex_v2 PRIVATE
        CYW43_FW_STREAM=1
        RPC_CYW43_FW_BT=${RPC_CYW43_FW_BT}
        CYW43_CHIPSET_FIRMWARE_INCLUDE_FILE="cyw43_fw.h")
    # uzlib's extra checks on a damaged stream: two compares, worth having on
    # input that is only ever trusted after its CRC. One of them compares an int
    # with sizeof, safely (it tests < 0 first), and -Wextra says so.
    set_property(SOURCE ${OS_ROOT}/uzlib/tinflate.c APPEND PROPERTY
        COMPILE_DEFINITIONS UZLIB_CONF_PARANOID_CHECKS=1)
    set_property(SOURCE ${OS_ROOT}/uzlib/tinflate.c APPEND PROPERTY
        COMPILE_OPTIONS -Wno-sign-compare)
    # Runs once per radio start-up, so it is cold code like the rest of the driver.
    if(RPC_COLD_SIZE AND CMAKE_BUILD_TYPE STREQUAL "Release")
        set_property(SOURCE ${RPC_FW_STREAM_SOURCES} ${CYW43_FW_DIR}/cyw43_ll.c
            APPEND PROPERTY COMPILE_OPTIONS -Os)
    endif()
    message(STATUS "CYW43 firmware: stored deflated (os/patches/cyw43-fw-stream.patch)")
endif()

# --- SSH: a server to log in to, and a client to log in elsewhere ---------------
#
# Needs the radio and mbedTLS, so only a W board can have it; ON by default for
# the RP2350 one (pico2_w), OFF for pico_w, whose slot has too little left for
# it. pico2 has no radio at all, so the option means nothing there.
#
# The option is RPC_SSH, and like every feature its value comes from
# tools/features.py (the features section above): `./build.sh pico2_w --without
# ssh`, or `-DRPC_SSH=OFF`. It is switched off with Wi-Fi, which it runs on, so
# an image built without Wi-Fi has no SSH either. Without it, ssh_off.cpp keeps
# `ssh` on a board whose standard image has it, saying it was left out.
#
# Everything that speaks the protocol is in core/ and host-tested against a
# real OpenSSH (os/host/ssh_*); shell/sshd.cpp and sshc.cpp are the device side.
# All of it at -Os: none of it is on the supervisor-call path the -O3 is for,
# and the arithmetic underneath is mbedTLS, which is -Os already.
message(STATUS "SSH: ${RPC_SSH}")
if(RPC_SSH)
    set(RPC_SSH_SOURCES
        ${OS_ROOT}/core/sshwire.cpp
        ${OS_ROOT}/core/sshcrypto.cpp
        ${OS_ROOT}/core/sshtrans.cpp
        ${OS_ROOT}/core/sshserver.cpp
        ${OS_ROOT}/core/sshclient.cpp
        ${OS_ROOT}/core/sshsess.cpp
        ${OS_ROOT}/core/sshkeys.cpp
        ${OS_ROOT}/core/ed25519.cpp
        ${OS_ROOT}/shell/sshd.cpp
        ${OS_ROOT}/shell/sshc.cpp)
    target_sources(rpcortex_v2 PRIVATE ${RPC_SSH_SOURCES})
    # RPC_HAS_SSH (below, with the other features) is also seen by
    # mbedtls_config.h, which adds deterministic ECDSA for it.
    set_source_files_properties(${RPC_SSH_SOURCES} PROPERTIES COMPILE_OPTIONS -Os)
else()
    target_sources(rpcortex_v2 PRIVATE ${OS_ROOT}/shell/ssh_off.cpp)
endif()

# --- what the source sees of the features ----------------------------------------
#
# Each feature as a define that is ALWAYS 0 or 1 (RPC_HAS_<X>), so the source can
# test it in a plain `if` as well as an #if; whether this board could carry it at
# all (RPC_CAN_<X>), so a stand-in can say "this image was built without Wi-Fi"
# on a Pico W and "this board has no wireless hardware" on a Pico; the list `ver`
# prints; and the variant: "standard", or the name a custom image's file carries
# ("nobt"). An emulator image is never the standard one, so it says "emu" too.
# tools/release.py reads the variant back out of the built image and publishes
# nothing that is not "standard".
if(RPC_SSH)
    set(_rpc_has_SSH 1)
else()
    set(_rpc_has_SSH 0)
endif()
foreach(_f WIFI BLUETOOTH SD PERIPHERALS)
    if(RPC_FEATURE_${_f})
        set(_rpc_has_${_f} 1)
    else()
        set(_rpc_has_${_f} 0)
    endif()
endforeach()
string(REPLACE ";" " " _rpc_list "${RPC_FEATURE_LIST}")
set(_rpc_variant "${RPC_FEATURE_VARIANT}")
if(RPC_CONSOLE_UART)
    if(_rpc_variant)
        set(_rpc_variant "${_rpc_variant}-emu")
    else()
        set(_rpc_variant "emu")
    endif()
endif()
if(NOT _rpc_variant)
    set(_rpc_variant "standard")
endif()
message(STATUS "Features: ${_rpc_list} (image: ${_rpc_variant})")
target_compile_definitions(rpcortex_v2 PRIVATE
    RPC_HAS_WIFI=${_rpc_has_WIFI}
    RPC_HAS_BLUETOOTH=${_rpc_has_BLUETOOTH}
    RPC_HAS_SSH=${_rpc_has_SSH}
    RPC_HAS_SD=${_rpc_has_SD}
    RPC_HAS_PERIPHERALS=${_rpc_has_PERIPHERALS}
    RPC_CAN_WIFI=${RPC_CAN_WIFI}
    RPC_CAN_BLUETOOTH=${RPC_CAN_BLUETOOTH}
    RPC_CAN_SSH=${RPC_CAN_SSH}
    RPC_CAN_SD=${RPC_CAN_SD}
    RPC_CAN_PERIPHERALS=${RPC_CAN_PERIPHERALS}
    RPC_STD_SSH=${RPC_STD_SSH}
    RPC_FEATURES="${_rpc_list}"
    RPC_IMAGE_VARIANT="${_rpc_variant}")

# The package index rides along too, so `pkg available` works on a device that
# has never been online. v1 shipped its catalogue the same way. It goes stale
# between releases, which is what `pkg update` is for — but a fresh device
# should be able to see what exists before it can reach the network.
#
# WHERE IT COMES FROM. The package repository, when it is checked out beside this
# one, so a build always carries the current catalogue. Otherwise stock_index.json
# in this directory, a copy that is refreshed when a release is cut, so a fresh
# clone of this repository builds on its own (CI does, and so can anyone else).
set(STOCK_INDEX ${OS_ROOT}/../../RPCortex-repo/repo-v2/index.json)
if(NOT EXISTS ${STOCK_INDEX})
    set(STOCK_INDEX ${OS_ROOT}/stock_index.json)
endif()
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/stock_index.cpp
    COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
            ${STOCK_INDEX} ${CMAKE_BINARY_DIR}/stock_index.cpp stock_index
    DEPENDS ${STOCK_INDEX} ${OS_ROOT}/tools_bin2c.py
)
add_custom_target(stock_index_src DEPENDS ${CMAKE_BINARY_DIR}/stock_index.cpp)
target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_index.cpp)
add_dependencies(rpcortex_v2 stock_index_src)

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

# --- the composite USB device -----------------------------------------------
#
# The console is not the only thing on the bus any more: there is a drive too,
# and a vendor interface picotool uses to reboot the board into BOOTSEL. That
# needs a descriptor set the SDK cannot generate, so its own is compiled out and
# usb_descriptors.c supplies one. Everything else in pico_stdio_usb stays.
#
# This is a documented switch, not a workaround: the SDK guards its descriptor
# file with exactly this flag, for exactly this case.
# --- how the flash is divided -------------------------------------------------
#
# ONE definition, here, because two would drift. The firmware runs from the
# first half of this and an update is assembled in the second, and the
# filesystem is whatever the part has left over — so a slot larger than the
# image needs is storage nobody gets, doubled.
#
# Sized against the DEVELOPMENT build (--dev-packages), which is the largest an
# image gets, so a debugging build never fails to fit. Measured at build 413,
# in KB; build.sh prints the live release figure every build, which is the one
# that gates a release — these are the debugging worst case.
#
#   board     image   slot    spare   pkg-slots   filesystem
#   pico2_w   1019    1024        5         256         1792
#   pico_w     820     832       12           0          384
#   pico2      415     512       97         256         2816
#   pico       314     384       70           0         1280
#
# The pico-slots column is new since this table was first written; it comes out
# of the filesystem, which is why the pico2_w filesystem is 1792 and not 2048.
# The spare column is the one to watch: pico2_w's dev build sits 5 KB under the
# wall. At build 412 it was 9 KB OVER — a measured 1058040 B against the 1048576 B
# slot, so `build.sh pico2_w --dev-packages` failed its own slot check. The
# scanf/strtod size pass at 413 is what brought the debugging build back inside
# its slot. The next feature that lands on all boards has to keep an eye on this
# row, not just the release headroom.
#
# The wireless images carry the CYW43 firmware (148 KB stored deflated, see
# "the radio firmware, stored deflated" above; 227 KB as the SDK ships it) and
# cannot be made much smaller. The plain ones have no radio and no business reserving room for one.
#
# Written out as well as compiled in, so build.sh checks the image against what
# the firmware ACTUALLY holds rather than against a second copy of this table.
# A slot check that reads its own guess is not a check.
if(PICO_BOARD STREQUAL "pico_w")
    set(RPC_FW_RESERVE 1703936)      #  832 KB slot, 384 KB filesystem
elseif(PICO_BOARD STREQUAL "pico")
    set(RPC_FW_RESERVE 786432)       #  384 KB slot, 1280 KB filesystem
elseif(PICO_BOARD STREQUAL "pico2_w")
    set(RPC_FW_RESERVE 2097152)      # 1024 KB slot, 2048 KB filesystem
else()
    set(RPC_FW_RESERVE 1048576)      #  512 KB slot, 3072 KB filesystem
endif()
target_compile_definitions(rpcortex_v2 PRIVATE RPC_FW_RESERVE=${RPC_FW_RESERVE})
file(WRITE ${CMAKE_BINARY_DIR}/fw_reserve.txt "${RPC_FW_RESERVE}")

# --- flash slots that packages RUN from --------------------------------------
#
# A position-independent package's read-only half is executed in place, so only
# its writable half is ever resident: Nova D1 costs 60 KB of RAM instead of 180.
# That half has to live somewhere, and this is the region it lives in — between
# the firmware reserve and the filesystem, taken OUT of the filesystem.
#
# Sized against how the package has actually grown rather than against what it
# is today. Nova D1's read-only half went 84 KB, then 116 KB, then 124 KB over
# three releases; with the recipe and the header a slot holds about 157 KB now.
# 256 KB leaves real room; 192 would already be tight enough to need revisiting.
#
# ONE slot, not two. Erasing a slot means erasing the code a running copy is
# fetching instructions from, so an install has to unload the package first
# either way, and a second slot would be a quarter of a megabyte spent to avoid
# a reinstall that costs seconds.
#
# ZERO on the RP2040 boards, and not arbitrarily: a slot holds `svc` veneers that
# name firmware by ABI index, because a baked ADDRESS would be wrong after any
# firmware update. Those only work where packages run unprivileged, and
# sandbox_rp2.cpp compiles SANDBOX_SUPPORTED to 0 on ARMv6-M — so an RP2040 runs
# DIRECT veneers and could not use a slot if it had one. It also has the least
# room: pico_w's filesystem is 384 KB and cannot spare a quarter of a megabyte.
# Both boards keep the copy-to-RAM path, which is unchanged.
set(RPC_PKG_SLOT_BYTES 262144)
if(PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico")
    set(RPC_PKG_SLOT_COUNT 0)
else()
    set(RPC_PKG_SLOT_COUNT 1)
endif()
math(EXPR RPC_PKG_REGION "${RPC_PKG_SLOT_COUNT} * ${RPC_PKG_SLOT_BYTES}")
target_compile_definitions(rpcortex_v2 PRIVATE
    RPC_PKG_SLOT_BYTES=${RPC_PKG_SLOT_BYTES}
    RPC_PKG_SLOT_COUNT=${RPC_PKG_SLOT_COUNT})
file(WRITE ${CMAKE_BINARY_DIR}/pkg_region.txt "${RPC_PKG_REGION}")

target_compile_definitions(rpcortex_v2 PRIVATE PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0)

# Where tud_task() runs. The whole reasoning is in os/usbdev.cpp; the short
# version is that mass-storage callbacks cannot run in interrupt context — they
# need the filesystem lock, which yields — and the MSC interface offers no
# deferral that does not spin inside the interrupt it is trying to escape. So
# the background task goes away and the `usb` task drives the device stack from
# thread context instead.
#
# Turning this back on without moving the MSC callbacks somewhere else is a
# hang, not a performance change.
target_compile_definitions(rpcortex_v2 PRIVATE PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0)

# The drive itself. msc_device.c is compiled into the TinyUSB device library
# unconditionally and its body is guarded by this, so turning the class on is a
# definition rather than a source change.
#
# The endpoint buffer is a whole sector: below that, TinyUSB calls the read and
# write callbacks with a partial sector and an offset, and every one of them
# becomes a separate trip through littlefs for part of a block.
target_compile_definitions(rpcortex_v2 PRIVATE
    CFG_TUD_MSC=1
    CFG_TUD_MSC_EP_BUFSIZE=512)

# The keyboard. Always compiled in, like the drive: the composite is static and
# the mode gate (usbmode.h) is what keeps the drive and the keyboard from being
# active at the same time, not their presence. The endpoint buffer holds one
# 8-byte boot-keyboard report — [modifier, reserved, six keys].
target_compile_definitions(rpcortex_v2 PRIVATE
    CFG_TUD_HID=1
    CFG_TUD_HID_EP_BUFSIZE=8)

# THE CONSOLE: USB on real hardware, UART under the emulator.
#
# The device's console is USB CDC, which is right — it is the one cable a Pico
# already has and it needs no adapter. The RP2040 model this project runs under
# Renode does not implement USB at all, but does implement the PL011 UART, so an
# emulator build puts the console there instead and everything above it is
# unchanged: stdio_init_all picks whichever is compiled in, and the shell reads
# through getchar either way.
#
#     ./build.sh pico --emu
#
# This is the only difference between the image on the bench and the image under
# the emulator, which is what makes the emulator worth trusting.
# ADDED to USB, not swapped for it.
#
# Switching stdio to UART alone takes pico_stdio_usb out of the link, and with
# it the tinyusb headers that usbmsc.cpp and usb_descriptors.c include directly
# — the transfer area is USB whether or not the console is. So the emulator
# build keeps USB exactly as the board has it and turns the UART on as well.
# The SDK's stdio multiplexes both directions across every registered driver,
# so the shell reads from whichever one a character arrives on.
#
# The consequence is the one that matters: the image under the emulator differs
# from the image on the bench by ONE enabled output, not by a different console
# stack. That is what makes what happens here worth believing about there.
option(RPC_CONSOLE_UART "Also put the console on UART0, for emulation" OFF)
pico_enable_stdio_usb(rpcortex_v2 1)
if(RPC_CONSOLE_UART)
    pico_enable_stdio_uart(rpcortex_v2 1)
    target_compile_definitions(rpcortex_v2 PRIVATE RPC_CONSOLE_UART=1)
else()
    pico_enable_stdio_uart(rpcortex_v2 0)
endif()
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
# NO -ffunction-sections / -fdata-sections, and that is the opposite of the usual
# advice for a reason: they exist so a LINKER can discard what nothing reaches,
# and a package is never linked. All they do here is put every function and every
# string in a section of its own — which the loader then has to map, relocate and
# account for individually.
#
# Measured on httpd: 83 sections with them, 13 without, and the object drops from
# 20144 to 13808 bytes. LOADER_MAX_SECTIONS is 128, so they were a ceiling on how
# large a package could be at all — around a thousand lines — for no benefit
# whatsoever. W^X is unaffected: the merged .rodata is still flagged A, not WA,
# so the loader's SHF_WRITE sort still puts it on the read-only side.
#
# Built for cortex-m0plus (ARMv6-M) deliberately: ARMv8-M Baseline is a superset,
# so one binary serves the RP2040 and RP2350 boards alike.
set(APP_FLAGS -mthumb -mcpu=cortex-m0plus -Os -std=c++17
    -fno-exceptions -fno-rtti -fno-common -fno-jump-tables
    -fno-use-cxa-atexit -I${OS_ROOT}/include)

# A package may be one .cpp or many.
#
# One file compiles straight to the .app, as it always did. Several are compiled
# separately and then PARTIALLY linked (`ld -r`) into a single relocatable object
# — which is still ET_REL, still carries only the three relocation types the
# loader handles, and looks to it exactly like a big single-file package.
#
# This exists because Nova D1 is not going to be one file, and the alternative —
# a unity build stitched together with #include of .cpp files — hides the
# dependency graph the layer rule depends on being visible.
# rpc_add_app(name [PIC])
#
# PIC opts one package into position-independent code: .text and .rodata carry no
# absolute address, so they can eventually run from flash and only .data/.bss plus
# a GOT stay resident. It is OPT-IN per package on purpose while it is proven —
# the default path stays byte-for-byte what it was, so a PIC package and a plain
# one can be compared side by side. See notes/xip-packages.md and the GOT handling
# in loader-spike/firmware/loader.cpp.
function(rpc_add_app name)
    file(GLOB app_srcs CONFIGURE_DEPENDS ${OS_ROOT}/apps/${name}/*.cpp)
    if(NOT app_srcs)
        message(FATAL_ERROR "rpc_add_app(${name}): no .cpp files in apps/${name}/")
    endif()

    set(this_flags ${APP_FLAGS})
    if("${ARGN}" MATCHES "(^|;)PIC($|;)")
        list(APPEND this_flags -fPIC -msingle-pic-base -mno-pic-data-is-text-relative)
    endif()

    list(LENGTH app_srcs app_nsrc)
    if(app_nsrc EQUAL 1)
        set(app_build COMMAND ${CMAKE_CXX_COMPILER} ${this_flags}
                              -c ${app_srcs} -o ${CMAKE_BINARY_DIR}/apps/${name}.app)
    else()
        # Compile each into apps/<name>.d/, then fold them into one object. The
        # object list is built in the same order every time so the .app is
        # reproducible rather than depending on the order the glob came back in.
        list(SORT app_srcs)
        set(app_build COMMAND ${CMAKE_COMMAND} -E make_directory
                              ${CMAKE_BINARY_DIR}/apps/${name}.d)
        set(app_objs "")
        foreach(src ${app_srcs})
            get_filename_component(stem ${src} NAME_WE)
            set(obj ${CMAKE_BINARY_DIR}/apps/${name}.d/${stem}.o)
            list(APPEND app_build COMMAND ${CMAKE_CXX_COMPILER} ${this_flags}
                                          -c ${src} -o ${obj})
            list(APPEND app_objs ${obj})
        endforeach()
        # --force-group-allocation and the script are BOTH needed, and the
        # reason is in apps_partial.ld: without them a C++ package grows two ELF
        # sections per weak symbol and runs into the loader's limit of 128 long
        # before it runs into anything else. Measured on the Nova D1 skeleton:
        # 31 sections plain, 23 with the flag alone, 30 with the script alone,
        # 14 with both — and flat from there however many classes it gains.
        list(APPEND app_build COMMAND ${CMAKE_LINKER} -r --force-group-allocation
                                      -T ${OS_ROOT}/apps_partial.ld
                                      -o ${CMAKE_BINARY_DIR}/apps/${name}.app ${app_objs})
    endif()

    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/apps/${name}.app
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/apps
        ${app_build}
        # Check every symbol resolves NOW, not when someone loads it on a device.
        # An unresolved symbol otherwise surfaces as a load failure naming a
        # function the author never wrote. For a multi-file package this also
        # catches the one a partial link cannot: a declaration nothing defines.
        COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_checkapp.py
                ${CMAKE_BINARY_DIR}/apps/${name}.app
                ${OS_ROOT}/api.cpp
                ${CMAKE_NM}
        # And that no two of the package's files share a file-scope name.
        # Separate translation units make that legal on the device and it is
        # a redefinition in novagui_test, which compiles the whole package into
        # one unit so it can drive the runner. Three separate merges have now
        # produced one — g_out, DeviceScreen, shell_task, Pending — each time
        # discovered as a wall of template errors in a harness nobody had run
        # yet. Refused here instead, naming both files.
        COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_checkuniq.py
        # Then make it smaller (tools_packapp.py has the detail): names the
        # loader never reads are dropped, 18% of Nova D1. The full object stays
        # beside it as <name>.dbg, for objdump and addr2line.
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/apps/${name}.app
                ${CMAKE_BINARY_DIR}/apps/${name}.dbg
        COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_packapp.py
                ${CMAKE_BINARY_DIR}/apps/${name}.dbg
                ${CMAKE_BINARY_DIR}/apps/${name}.app
                ${CMAKE_OBJCOPY}
        DEPENDS ${app_srcs} ${OS_ROOT}/include/rpc_app.h
                ${OS_ROOT}/api.cpp ${OS_ROOT}/tools_checkapp.py
                ${OS_ROOT}/tools_checkuniq.py ${OS_ROOT}/tools_packapp.py
        VERBATIM)
    add_custom_target(app_${name} ALL DEPENDS ${CMAKE_BINARY_DIR}/apps/${name}.app)
endfunction()

rpc_add_app(calc)
rpc_add_app(ws2812)
rpc_add_app(dht)
rpc_add_app(probe)
rpc_add_app(gpio)
rpc_add_app(i2cscan)
# greet is the position-independent exemplar while task #93 is proven: the
# smallest real package, opted in so realapp_test exercises the r9/GOT path on
# every host run. Every other package stays on the default (copy-to-RAM) path.
#
# Proven on a Pico 2 W, both routes, 2026-08-09: loaded by copy-to-RAM at 576 B
# resident and from a flash slot at 160 B, registering its command and running
# it correctly either way.
# NOT PIC, on purpose. There is one package slot per board and the first
# claimant keeps it, so a worked example that saves 416 bytes by taking it
# would cost novad1 the 112 KB the slot is there for. greet proves the loader
# on the ordinary path, which is the path nearly every package uses.
rpc_add_app(greet)
rpc_add_app(tuidemo)
rpc_add_app(stress)
rpc_add_app(bench)
rpc_add_app(websearch)
rpc_add_app(speedtest)
rpc_add_app(havoc)
rpc_add_app(httpd)
rpc_add_app(backup)
rpc_add_app(fileexp)
rpc_add_app(sysmon)
# novad1 is the package task #93 exists for: 114 KB of read-only half, more than
# the 89 KB largest free block a booted board has, so it cannot be loaded onto a
# running device. Built PIC, that half is a position-independent blob for a flash
# slot and only the ~60 KB writable half stays resident.
#
# NOT PIC yet, and the reason is no longer the one recorded here.
#
# The fault that took novad1 back out of PIC —
#
#     HARD FAULT in novad1  pc=0x2003eef8  lr=0x2003ef01  IACCVIOL
#
# — was blamed on -fPIC moving const pointer tables into the writable half,
# which W^X maps non-executable. That was wrong, twice over. The pc is not in
# the package's code at all: it is the `bx r3` in the enter gate, at
# veneers+24, with lr holding the exit gate at veneers+32 — the first
# instruction a sandboxed package executes after privilege is dropped. And it
# was not the copy-to-RAM path. It was the flash slot, whose gate pool was 48
# bytes, which the protection unit cannot describe, so the region was silently
# left off. app_pic_load rounds it now; see the note there.
#
# With that fixed, both were proved on a Pico 2 W. A PIC novad1 installs into
# the slot, loads from it, answers `novad1 status` and `novad1 scan`, and
# composes a frame onto a real SSD1309 — at 62272 B resident against 174400 B
# copied into RAM, which is 112 KB of a 291 KB heap given back.
#
# ON, and what settled it was measuring the alternative rather than assuming it.
#
# This was held off on the grounds that a PIC novad1 which could not get the
# slot would need 126 KB in one piece, more than a booted device has — turning
# a package that loads into one that does not. The second half of that is
# false. The PLAIN build needs 121,728 B in one piece, and the largest free
# block is 84 KB booted, 108 KB after stopping the service and unloading. So
# non-PIC does not install on a running device either; it only ever managed it
# at boot, when the heap is still whole.
#
# Against that, through the slot an install peaks at 70 KB with a largest
# single allocation of 14.5 KB. PIC is not a trade here — it is better when the
# slot is free and no worse when it is not, because neither build fits then.
#
# The one real hazard is contention: one slot per board, first claimant keeps
# it. So greet is deliberately NOT PIC (see above) — an example package must
# not be able to take the slot from the package that needs it. Should a second
# package ever want one, that is when `pkg` needs an eviction rule, and the
# question can be answered with a case in front of it.
#
# This also widens the test net: check_pic had only greet, 304 bytes, to prove
# the whole slot path with. Now it has this.
rpc_add_app(novad1 PIC)

# --- packages that ship installed -------------------------------------------
#
# The .app bytes are baked into the firmware and written into /pkg on a first
# boot. That is what keeps them real packages — `pkg remove stress` deletes it
# like any other, and it stays gone — rather than built-in commands wearing a
# package's clothes.
# --- the development test suite ---------------------------------------------
#
# bench, probe, stress and havoc exist to exercise THIS build. They measure what the
# host tests cannot — real cores, real interrupt jitter, real timing — and a
# device with none of them attached cannot tell whether it is healthy.
#
# havoc is the odd one: the other three check that things WORK, and it checks
# that bad input FAILS SAFELY. It belongs here for the same reason as the rest,
# and one more — a package that hunts for ways to crash the device is not
# something to publish to a repo people install from.
#
# They are baked in rather than published, for three reasons. They are of no use
# to anyone who just wants the OS; publishing each one costs a build, a checksum
# and an index entry every time it changes; and a device that has them without
# having to fetch them is a device that can be diagnosed before the network is
# working, which is exactly when it usually matters.
#
# OFF unless asked for, and asking is one flag: they are the `devpkgs` feature
# (tools/features.py), so RPC_DEV_PACKAGES comes from the features section above
# like every other one, and an image with them is a custom image that a release
# refuses:
#
#     ./build.sh pico2_w --with devpkgs        (or --dev-packages)
#

if(RPC_DEV_PACKAGES)
    target_compile_definitions(rpcortex_v2 PRIVATE RPC_DEV_PACKAGES=1)
    foreach(devpkg bench stress probe havoc)
        add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/stock_${devpkg}.cpp
            COMMAND ${CMAKE_COMMAND} -E env python3 ${OS_ROOT}/tools_bin2c.py
                    ${CMAKE_BINARY_DIR}/apps/${devpkg}.app
                    ${CMAKE_BINARY_DIR}/stock_${devpkg}.cpp stock_${devpkg}
            DEPENDS ${CMAKE_BINARY_DIR}/apps/${devpkg}.app ${OS_ROOT}/tools_bin2c.py
            VERBATIM)
        add_custom_target(stock_${devpkg}_src DEPENDS ${CMAKE_BINARY_DIR}/stock_${devpkg}.cpp)
        target_sources(rpcortex_v2 PRIVATE ${CMAKE_BINARY_DIR}/stock_${devpkg}.cpp)
        # ORDERED, not merely both required.
        #
        # app_${devpkg} and this target both depend on apps/${devpkg}.app, and
        # with a Makefiles generator two independent targets that need the same
        # custom-command output can each run its rule — at the same time, into
        # the same file. It fails maybe one build in five, always on a different
        # package, and reads as a compiler error in something untouched.
        #
        # Naming app_${devpkg} as a dependency of this target, rather than only
        # as a second dependency of the firmware, is what serialises them.
        add_dependencies(stock_${devpkg}_src app_${devpkg})
        add_dependencies(rpcortex_v2 stock_${devpkg}_src app_${devpkg})
    endforeach()
endif()
