*** Comments ***
littlefs used from two tasks at once, then checked.
#
#     emu/run-tests.sh emu/tests/fs_concurrency.robot
#
# The scheduler task appends a line to one file every second while the shell's
# own task reads another file over and over. They are separate tasks, and the
# scheduler is free to run on the other core. Afterwards everything has to add
# up: every read of the read-only file came back whole, the appended file holds
# only whole lines, df is sane, and fscheck --scan reads every file back.
#
# Deterministic in what it asserts, and bounded: three rounds of ten reads, each
# checked by its exact line, word and byte count, a one-second pause after each
# round, and a wall-clock timeout on the whole test. How many appends land is up
# to the clock, so the appended file is checked against invariants that hold for
# any number of them (one word and two bytes a line) rather than a count.
#
# That the writer really ran during the read phase is checked, not assumed: the
# appended file is counted before and after it, and the test fails unless it
# grew. A run in which the writer never fired has tested nothing.
#
# The reads are typed commands rather than a .rps loop, so every single read
# can be checked here. (Scripts were also unusable when this was written: a
# script run by `bg` overflowed its stack and silenced the machine. That is
# fixed and has its own suite, bg_script.robot.)
#
# The read file goes over with `put`. put acknowledges every 256 bytes with 0x06
# for a sender on real hardware to wait on; the emulated UART queues everything
# it is given, so the bytes are sent in one go.
#
# It needs a shell, so it drives first-run setup and the login the way a person
# would. A boot that never reaches setup fails at the first wait, which is
# boot.robot's territory; this file is about what happens after it.

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
${ROUNDS}   3
${READS}    10

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
A Writer And A Reader Leave The Filesystem Consistent
    # Wall-clock, unlike the terminal tester's timeouts, which count emulated
    # time. A wedged core slows emulated time to a crawl, and without this a
    # 60-second wait can still be waiting most of an hour later.
    [Timeout]                 10 minutes
    Boot RPCortex
    Set Up And Log In

    # A known-good layout to start from. --fix rather than a plain check, so a
    # layout problem that has nothing to do with concurrency is repaired here
    # instead of failing the check after the race. (Setup once left the two
    # home directories missing until the next boot; it makes them now, and
    # bg_script.robot checks that.)
    Type                      fscheck --fix
    Wait For Line On Uart     Repaired\\.|Everything is where it should be\\.    treatAsRegex=true
    Done

    # 64 lines, "r0:" .. "r63:" each followed by the same 36 characters: 2614
    # bytes, so one read crosses ten of littlefs's 256-byte cache lines.
    ${data}=                  Evaluate    ''.join('r%d:abcdefghijklmnopqrstuvwxyz0123456789\\n' % i for i in range(64))
    Run                       mkdir /emu
    Put File                  /emu/r.txt       ${data}
    ${g}=                     Wc Of            /emu/r.txt
    Should Be Equal           ${g}[0]:${g}[1]:${g}[2]    64:64:2614

    # The writer: the scheduler task, appending once a second. Quoted, so the
    # redirect is stored as part of the job instead of being applied to `task`.
    Run                       echo w > /emu/w.txt
    Prove Sudo
    Run                       sudo task add 1 "echo w >> /emu/w.txt"
    Type                      sudo task start
    Wait For Line On Uart     Scheduler running
    Done

    # The reader: the shell's own task, while the writer runs. Every read has
    # to see the whole file. A typed read takes a few tens of milliseconds of
    # emulated time, so ten of them fit between two firings of a once-a-second
    # writer; the second's pause after each round is what puts firings inside
    # the read phase rather than before or after it.
    ${before}=                Wc Of            /emu/w.txt
    FOR    ${round}    IN RANGE    ${ROUNDS}
        FOR    ${k}    IN RANGE    ${READS}
            ${g}=             Wc Of            /emu/r.txt
            Should Be Equal   ${g}[0]:${g}[1]:${g}[2]    64:64:2614
            ...               msg=read ${round}.${k} of r.txt came back as ${g}
        END
        Run                   sleep 1
    END
    ${after}=                 Wc Of            /emu/w.txt
    Log                       w.txt: ${before}[0] lines before the reads, ${after}[0] after
    Should Be True            ${after}[0] > ${before}[0]    msg=the writer never ran while the reader did

    # Stop the writer, and give a firing already under way time to finish.
    Type                      sudo task stop
    Wait For Line On Uart     Scheduler stopping
    Done
    Run                       sleep 2

    # Whatever number of appends landed, each one is whole: one word and two
    # bytes a line. A torn or doubled append breaks the ratio.
    ${w}=                     Wc Of            /emu/w.txt
    Should Be True            ${w}[0] >= ${after}[0]
    Should Be Equal           ${w}[1]    ${w}[0]
    Should Be True            ${w}[2] == 2 * ${w}[0]

    # df adds up: the whole 1280 KB filesystem, used and free summing to it
    # (give or take the rounding of each to whole KB), and well short of the
    # 95% warning. A fresh device with these files on it sits around 6%.
    Type                      df
    Wait For Line On Uart     Total : 1280 KB
    ${u}=                     Wait For Line On Uart    Used\\s+:\\s+(\\d+) KB\\s+\\((\\d+)%\\)    treatAsRegex=true
    ${f}=                     Wait For Line On Uart    Free\\s+:\\s+(\\d+) KB                   treatAsRegex=true
    Done
    Should Be True            1279 <= ${u}[Groups][0] + ${f}[Groups][0] <= 1280
    Should Be True            0 < ${u}[Groups][1] < 50

    Type                      fscheck
    Wait For Line On Uart     Everything is where it should be.
    Done
    Type                      fscheck --scan
    Wait For Line On Uart     Every file read back cleanly.
    Done
