*** Comments ***
The service supervisor restarts a service that keeps failing.
#
#     emu/run-tests.sh emu/tests/service_restart.robot
#
# A service whose command fails is started again after a pause (2 s, then 4,
# then 8 ...) and each restart is logged as a warning. This waits for the first
# two rather than the full five-and-give-up, which would be a minute of emulated
# time and several of wall clock.

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

# Typing a command, and knowing when it has finished.
#
# Waiting for the prompt does not work: the line editor redraws the whole prompt
# on every character it echoes, so "root@vela" is on the wire long before the
# command has run. Every command is followed by `; echo =done=` instead, and the
# wait is for that at the START of a line. The echoed command line contains
# "=done=" too, but never at the start, which is what the anchor is for.
Type
    [Arguments]               ${line}
    Write Line To Uart        ${line} ; echo =done=    waitForEcho=false

Done
    Wait For Line On Uart     ^=done=      treatAsRegex=true

Run
    [Arguments]               ${line}
    Type                      ${line}
    Done

# Services and scheduled tasks change what runs later, so adding or starting
# them takes sudo. One `sudo -v` here, with the password at its prompt, counts
# for five minutes of emulated time, which is longer than the test.
Prove Sudo
    Type                      sudo -v
    Answer                    Password for root    ${PW}
    Done

Put File
    [Arguments]               ${path}      ${text}
    ${n}=                     Get Length   ${text}
    Type                      put ${path} ${n}
    Wait For Line On Uart     Send ${n} raw bytes now
    Write To Uart             ${text}
    Wait For Line On Uart     Wrote ${path} (${n} bytes)
    Done

# `wc` prints "  N lines  N words  N bytes". Two spaces are a cell boundary to
# Robot, so the pattern is a regex rather than literal text. Returns the three
# numbers.
Wc Of
    [Arguments]               ${path}
    Type                      wc ${path}
    ${m}=                     Wait For Line On Uart    (\\d+) lines\\s+(\\d+) words\\s+(\\d+) bytes
    ...                       treatAsRegex=true
    Done
    RETURN                    ${m}[Groups]

*** Test Cases ***
A Failing Service Is Started Again
    [Timeout]                 10 minutes
    Boot RPCortex
    Set Up And Log In

    # A command that fails every time: cat of a file that is not there.
    Prove Sudo
    Run                       sudo service add cat /emu/not-here
    Type                      sudo service start
    Wait For Line On Uart     cat /emu/not-here
    Done

    # 2 s to the first restart, 4 more to the second, but exactly when depends
    # on when the supervisor next looks. So poll the status until it says two,
    # rather than sleeping a fixed time and hoping: `log` is a one-off dump, and
    # a restart that lands after it is never printed.
    ${n}=                     Set Variable    0
    FOR    ${try}    IN RANGE    15
        Run                   sleep 2
        Type                  service status
        ${m}=                 Wait For Line On Uart    pid -?\\d+\\s+(\\d+) restarts?\\s+cat /emu/not-here    treatAsRegex=true
        Done
        ${n}=                 Convert To Integer    ${m}[Groups][0]
        IF    ${n} >= 2    BREAK
    END
    Should Be True            ${n} >= 2    msg=the supervisor restarted the service ${n} times in 30 s

    # And each restart is in the log, in order.
    Type                      log
    Wait For Line On Uart     service restarted after failing \\(1 of 5\\)    treatAsRegex=true
    Wait For Line On Uart     service restarted after failing \\(2 of 5\\)    treatAsRegex=true
    Done
