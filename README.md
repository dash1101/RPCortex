![RPCortex](Assets/RPCortex.png)

# What is RPCortex?
RPCortex is a lightweight customizable and capable operating-system written in c++ for the RP2040 Processor

## Features
As of now, the features are the following:\
TFT Display Support\
Command Line Interface, Like DOS\
WiFi Support coming soon\
SD Support coming soon\
More info can be forund in the Changelog below.

## Instalation
As of now, RPCortex is in an alpha stage, meaning that there arer different steps for installing each version.\
The current latest release 'v0.2.0-a' can be installed by:\
Installing Arduino IDE and the MBED OS for rp2040 in the boards manager\
In arduino IDE install the TFT_eSPI driver\
Download the rpc.ver.ino file and open it in Arduino IDE\
Modify the settings to your likings\
Then select your board and upload it!

## Changelog All Versions and Future Releases:
### v0.4.0-alpha
Release date: 2024, Late March - Early April\
❌Some multi threading support\
❌Force quit active command / program via serial. To do this, send (kill -SIGINT)

### **_`v0.3.5-alpha`_**
Release date: 2024, Mid March - Late March\
❌Download commands for pico-w\
❌RGB LED support\
❌Morse LED\
❌Button Macro support (ONLY 1 AS OF NOW)\
❌Joystick support for GUI APPS!!! (None as of now, but v1.0.0-rc will have some I'm sure, as well as touch support.)

### v0.3.0-a
❌ Some apps/cmds which include:\
ls/dir \
❌ Improvments on file system, and commands\
🟨 **External flash support**

### v0.2.5-a
❌ **Internal flash fs (NO LS/DIR)**\
🟨 Some apps/cmds which include:\
`cat, dir, mkdir, rmdir, del, write, help, chdir, cd`

### Previous Versions
✅ ALL THE COLORS!!!\
✅ Print drivers can print without line now\
✅ Morse code with beeper\
✅ **More stable, Display and System**\
✅ Easier to setup\
✅ On the fly setup for some objects\
✅ Clear screen for TFT\
✅ Print command\
✅ beep command\
✅ More stable display adapter\
✅ Concept display adapter\
✅ Concept command executer\
✅ An idea of what the future of RPC will look like


#### License
RPCortex is licensed under the MIT License.
