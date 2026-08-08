*** Comments ***
The OS booting, asserted rather than eyeballed.
#
# Renode's Robot integration attaches to the emulated UART and can both read
# lines and write them, so this is the real firmware being driven the way a
# person drives it — not a host build with the hardware stubbed out.
#
#     emu/run-tests.sh
#
# WHAT IT COVERS TODAY is the boot path: the banner, both cores coming up, and
# littlefs being formatted and mounted on emulated flash. That is where three of
# the last four bugs that reached a desk actually lived.
#
# WHAT IT DOES NOT COVER YET is the shell, and the reason is written down in
# emu/README.md: the first-run wizard prompts without a newline and the
# interaction has not been made reliable. The keywords for it exist and are used
# below; getting them to land is the next piece of work, not a missing feature
# of Renode.

*** Settings ***
Suite Setup                   Setup
Suite Teardown                Teardown
Test Setup                    Reset Emulation
Resource                      ${RENODEKEYWORDS}

*** Variables ***
${V2}       ${CURDIR}/../..
${ELF}      ${V2}/os/build_pico-emu/rpcortex_v2.elf
${PLAT}     ${CURDIR}/../Renode_RP2040

*** Keywords ***
Boot RPCortex
    Execute Command           $machine_name?="rpcortex"
    Execute Command           $platform_file?=@${PLAT}/boards/raspberry_pico.repl
    Execute Command           include @${PLAT}/boards/initialize_custom_board.resc
    Execute Command           sysbus LoadELF @${ELF}
    Execute Command           sysbus.cpu0 VectorTableOffset 0x00000000
    Execute Command           sysbus.cpu1 VectorTableOffset 0x00000000

    # QUIET THE BUS LOG, and this is not a tidiness preference.
    #
    # The OS reads the RTC control register as part of its always-on clock. The
    # RP2040 model does not implement that register, so every read emits a
    # warning — thousands a second — and Renode spends its time formatting log
    # lines instead of executing instructions. Left on, a boot that takes seven
    # seconds does not finish inside seven minutes.
    Execute Command           logLevel 3 sysbus

    Create Terminal Tester    sysbus.uart0    timeout=60
    Start Emulation

*** Test Cases ***
Boots To A Mounted Filesystem
    Boot RPCortex
    Wait For Line On Uart     RPCortex v2.0.0
    Wait For Line On Uart     POST
    # A REAL filesystem on emulated flash, written and read back through the
    # XIP/SSI model rather than a stub. A first boot has nothing there and
    # formats; the interesting assertion is that it mounts afterwards.
    Wait For Line On Uart     Filesystem mounted
    Wait For Line On Uart     Reading the registry
