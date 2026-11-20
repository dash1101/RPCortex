*** Comments ***
An image says what it was built with, and a command whose feature is not in it
says so instead of failing in some other way.
#
#     emu/run-tests.sh emu/tests/features.robot
#
# Works on any Pico image, standard or custom, which is the point: build one
# without the add-on drivers and run the same test against it.
#
#     ./build.sh pico --emu --without peripherals
#     emu/run-tests.sh emu/tests/features.robot \
#         --variable ELF:$PWD/os/build_pico-noperiph-emu/rpcortex_v2.elf
#
# `ver` must print a Features line. A Pico can never have Wi-Fi, so `wifi`
# must say the board has no wireless hardware. And `nfc` must say the add-on
# drivers were left out exactly when the Features line does not list them; on
# an image that has them it must NOT say that (with no reader attached it goes
# on to report the missing reader, which is not checked here).

*** Settings ***
Suite Setup                   Setup
Suite Teardown                Teardown
Test Setup                    Reset Emulation
Resource                      ${RENODEKEYWORDS}

*** Variables ***
${V2}       ${CURDIR}/../..
${ELF}      ${V2}/os/build_pico-emu/rpcortex_v2.elf
${PLAT}     ${CURDIR}/../Renode_RP2040
${PW}       emutest

*** Keywords ***
Boot RPCortex
    Execute Command           $machine_name?="rpcortex"
    Execute Command           $platform_file?=@${PLAT}/boards/raspberry_pico.repl
    Execute Command           include @${PLAT}/boards/initialize_custom_board.resc
    Execute Command           sysbus LoadELF @${ELF}
    Execute Command           sysbus.cpu0 VectorTableOffset 0x00000000
    Execute Command           sysbus.cpu1 VectorTableOffset 0x00000000
    # Load-bearing, not tidiness: see boot.robot.
    Execute Command           logLevel 3 sysbus
    Create Terminal Tester    sysbus.uart0    timeout=60
    Start Emulation

Answer
    [Arguments]               ${prompt}    ${reply}
    Wait For Prompt On Uart   ${prompt}
    Write Line To Uart        ${reply}     waitForEcho=false

Set Up And Log In
    Wait For Line On Uart     First Run Setup
    Answer                    Set a password for 'root'    ${PW}
    Answer                    Confirm password             ${PW}
    Wait For Line On Uart     Root account created
    Answer                    Owner name                   ${EMPTY}
    Answer                    Device name                  ${EMPTY}
    Wait For Line On Uart     No WiFi hardware detected
    Answer                    Timezone offset              ${EMPTY}
    Wait For Line On Uart     All set!
    Answer                    Username                     root
    Answer                    Password                     ${PW}
    Wait For Line On Uart     Welcome, root!

Type
    [Arguments]               ${line}
    Write Line To Uart        ${line} ; echo =done=    waitForEcho=false

Done
    Wait For Line On Uart     ^=done=      treatAsRegex=true

*** Test Cases ***
Says What It Was Built With
    [Timeout]                 10 minutes
    Boot RPCortex
    Set Up And Log In

    Type                      ver
    ${m}=                     Wait For Line On Uart    Features: ([a-z ]+)    treatAsRegex=true
    Done
    ${features}=              Set Variable    ${m}[Groups][0]
    Log To Console            Features in this image: ${features}

    Type                      wifi
    Wait For Line On Uart     This board has no wireless hardware.
    Done

    Type                      nfc
    IF    'peripherals' in '${features}'.split()
        Should Not Be On Uart    built without the add-on drivers    timeout=5
    ELSE
        Wait For Line On Uart    This image was built without the add-on drivers
    END
    Done
