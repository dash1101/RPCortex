*** Comments ***
A script run by `bg`, looping, from a background task.
#
#     emu/run-tests.sh emu/tests/bg_script.robot
#
# This used to take the machine down. The script interpreter kept about 5 KB of
# state on the stack before running a line, and every line runs the whole shell
# beneath it, so `bg script ...` on a 3 KB task stack overflowed at once (seen
# here at build 493). The state is on the heap now. The script appends twenty
# lines and then writes a marker; both are checked, and so is the shell.
#
# One test per file: a second boot in the same Renode, after Reset Emulation,
# stalled at the login prompt, which no suite here has needed to explain yet.

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
A Script Loops In The Background Without Overflowing
    [Timeout]                 10 minutes
    Boot RPCortex
    Set Up And Log In

    # First-run setup makes root's and guest's homes itself now. They used to
    # appear only at the next boot, so a plain fscheck here reported them gone.
    Type                      fscheck
    Wait For Line On Uart     Everything is where it should be.
    Done

    ${script}=                Evaluate    'set i 0\\nwhile lt $i 20\\necho x >> /emu/o.txt\\ninc i\\nend\\necho finished > /emu/done.txt\\n'
    Run                       mkdir /emu
    Put File                  /emu/s.rps       ${script}
    Type                      bg script /emu/s.rps
    Wait For Line On Uart     script /emu/s.rps
    Done

    # Give it time, then look: every line arrived, the script reached its end,
    # and the shell is still the shell.
    Run                       sleep 4
    ${o}=                     Wc Of            /emu/o.txt
    Should Be Equal           ${o}[0]:${o}[2]    20:40    msg=the background script's appends came back as ${o}
    Type                      cat /emu/done.txt
    Wait For Line On Uart     finished
    Done
    Type                      echo still-answering
    Wait For Line On Uart     ^still-answering    treatAsRegex=true
    Done
