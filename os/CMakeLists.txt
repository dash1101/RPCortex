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
    ${OS_ROOT}/core/httpparse.cpp
    ${OS_ROOT}/core/httpfetch.cpp
    ${OS_ROOT}/core/repoindex.cpp
    ${OS_ROOT}/core/preempt.cpp
    ${OS_ROOT}/core/joinguard.cpp
    ${OS_ROOT}/core/excframe.cpp
    ${OS_ROOT}/core/tui.cpp
    ${OS_ROOT}/core/tuikey.cpp
    ${OS_ROOT}/core/tuilist.cpp
    ${OS_ROOT}/core/rps.cpp
    ${OS_ROOT}/core/cmdline.cpp
    ${OS_ROOT}/core/interrupt.cpp
    ${OS_ROOT}/shell/power.cpp
    ${OS_ROOT}/shell/bt.cpp
    ${OS_ROOT}/shell/btaudio.cpp
    ${OS_ROOT}/shell/nfc.cpp
    ${OS_ROOT}/shell/ibutton.cpp
    ${OS_ROOT}/shell/cc1101.cpp
    ${OS_ROOT}/shell/sx1276.cpp
    ${OS_ROOT}/core/pcmring.cpp
    ${OS_ROOT}/core/wav.cpp
    ${OS_ROOT}/core/btname.cpp
    ${OS_ROOT}/core/nfcframe.cpp
    ${OS_ROOT}/core/onewire.cpp
    ${OS_ROOT}/core/powerpolicy.cpp
    ${OS_ROOT}/core/framebuf.cpp
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
    ${OS_ROOT}/shell/rollback.cpp
    ${OS_ROOT}/shell/text.cpp
    ${OS_ROOT}/shell/sys.cpp
    ${OS_ROOT}/shell/user.cpp
    ${OS_ROOT}/shell/help.cpp
    ${OS_ROOT}/shell/net.cpp
    ${OS_ROOT}/shell/netapps.cpp
    ${OS_ROOT}/shell/nettcp.cpp
    ${OS_ROOT}/shell/httptransport.cpp
    ${OS_ROOT}/shell/fetch.cpp
    ${OS_ROOT}/shell/ps.cpp
    ${OS_ROOT}/shell/logcmd.cpp
    ${OS_ROOT}/shell/usbdrive.cpp
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

# --- microSD: on the boards that have room for it ------------------------------
#
# The SD driver is an SPI block layer, a FAT16/32 reader and a `sd` command, and
# together they are a few kilobytes of flash. A few kilobytes is nothing on an
# RP2350 and it is a meaningful slice of what a Pico W has left:
#
#   pico2_w    85 KB headroom      pico2   170 KB headroom
#   pico_w     66 KB headroom      pico    119 KB headroom
#
# The reason it is off on the RP2040 boards is not only the budget, though. Nova
# D1 — the thing with a card slot wired to it — targets the RP2350 boards, and
# novaboard's own detect() refuses to claim an RP2040 board for exactly the same
# reason: the header matches and the part cannot carry the software. A Pico W
# with this compiled in would carry a driver for a slot it does not have.
#
# THE DEFINE HAS TO BE A DEFINE. PICO_CYW43_SUPPORTED is a CMake variable and
# testing it with #if was silently always false for a whole release — the note
# further down this file is about that exact bug. So RPC_HAS_SD is passed as a
# compile definition below, and `nm` on the two images is what proves it landed:
# a pico2_w image has sd_mount in it and a pico_w image does not.
if(PICO_BOARD STREQUAL "pico2_w" OR PICO_BOARD STREQUAL "pico2")
    set(RPC_HAS_SD ON)
else()
    set(RPC_HAS_SD OFF)
endif()
message(STATUS "microSD support: ${RPC_HAS_SD}")

if(RPC_HAS_SD)
    set(RPC_SD_SOURCES
        ${OS_ROOT}/core/sdproto.cpp
        ${OS_ROOT}/core/fatro.cpp
        ${OS_ROOT}/sdcard_rp2.cpp
        ${OS_ROOT}/shell/sd.cpp)
    target_sources(rpcortex_v2 PRIVATE ${RPC_SD_SOURCES})
    target_compile_definitions(rpcortex_v2 PRIVATE RPC_HAS_SD=1)

    # -Os for these four, against the -O3 the rest of an RP2350 image gets.
    #
    # The argument at the top of this file is that RP2350 has flash to spare and
    # the supervisor call is worth 37% of its speed. Neither half applies here.
    # Every one of these functions is waiting on a 12 MHz SPI bus — a block read
    # is 400 microseconds of clocking whatever the code around it was compiled
    # at — and the flash is not free after all: pico2_w has 85 KB of slot
    # headroom, and at -O3 this cost 16 KB of it.
    set_source_files_properties(${RPC_SD_SOURCES} PROPERTIES COMPILE_OPTIONS -Os)
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
    # Bluetooth. Both radios are the same chip, which is what makes this
    # possible at all — and Classic is the capability MicroPython could not
    # reach, so a media player is a v2 thing and was never a v1 one.
    target_link_libraries(rpcortex_v2
        pico_btstack_ble
        pico_btstack_classic
        pico_btstack_cyw43
        pico_btstack_run_loop_async_context
        pico_btstack_sbc_encoder)
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
# image gets, so a debugging build never fails to fit.
#
#   board     image   slot    spare   filesystem
#   pico2_w    916     1024     108       2048
#   pico_w     748      832      83        384
#   pico2      320      512     191       3072
#   pico       247      384     136       1280
#
# The wireless images carry the 227 KB CYW43 blob and cannot be made much
# smaller. The plain ones have no radio and no business reserving room for one.
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
        DEPENDS ${app_srcs} ${OS_ROOT}/include/rpc_app.h
                ${OS_ROOT}/api.cpp ${OS_ROOT}/tools_checkapp.py
                ${OS_ROOT}/tools_checkuniq.py
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
rpc_add_app(greet PIC)
rpc_add_app(tuidemo)
rpc_add_app(stress)
rpc_add_app(bench)
rpc_add_app(websearch)
rpc_add_app(speedtest)
rpc_add_app(havoc)
rpc_add_app(httpd)
# novad1 is the package task #93 exists for: 114 KB of read-only half, more than
# the 89 KB largest free block a booted board has, so it cannot be loaded onto a
# running device. Built PIC, that half is a position-independent blob for a flash
# slot and only the ~60 KB writable half stays resident. realapp_test proves the
# blob places anywhere and the writable half clears 89 KB. The device slot-install
# and load-from-slot wiring is the remaining stage-3 work; until it lands novad1
# still loads by copy-to-RAM, which is why this is a branch and not shipped.
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
# Turning them off is ONE flag, and that is deliberate — shipping the OS should
# not mean picking them back out of the build by hand:
#
#     ./build.sh --no-dev-packages
#
option(RPC_DEV_PACKAGES "Bake the development test suite into the image" ON)

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
