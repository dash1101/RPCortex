## Hyperion Commands:

#### Networking Commands:
- `wlan`
- `wlan connect`
- `wlan disconnect`
- `wlan scan`
- `wlan info`
- `wlan status`
- `wlan autoconnect`
- `ping [hostname]`
- `ifconfig`
- `dnslookup [hostname]`
- `curl [options] [url]`
- `wget [options] [url]`

#### System Commands:
- `temp` (Display temperature)
- `system` (System information)
- `reboot` (Reboot the OS)
- `shutdown` (Enter a low-power state and turn off the display)
- `uptime` (Show the system's uptime)
- `sleep` (Enter sleep mode)
- `exit` (Exit or safely crash the OS)
- `sysinfo` (Display detailed system information)
- `clear` (Clear the terminal screen)
- `cls` (Clear the terminal screen)

#### File System Commands:
- `ls` (List files in the current directory)
- `dir` (Alias for `ls`)
- `ls -l` (Detailed file list view)
- `write [filename]` (Write to a file)
- `mkdir [path]` (Create a new directory)
- `rmdir [path]` (Remove an empty directory)
- `delete [file_name]` (Delete a file)
- `del [file_name]` (Alias for `delete`)
- `read [filename]` (Read and display a file)
- `open [filename]` (Open and display a file)
- `view [filename]` (View a file’s content)
- `cat [filename]` (Display the content of a file)
- `edit [filename]` (Edit a file)
- `run [file]` (Run a script or executable file)
- `exec [code]` (Execute inline code)
- `launch [file]` (Launch a program)
- `start [file]` (Alias for `launch`)
- `rename [old_name] [new_name]` (Rename a file)
- `ren [old_name] [new_name]` (Alias for `rename`)
- `move [filename] [destination]` (Move a file)
- `cp [source] [destination]` (Copy a file)
- `copy [source] [destination]` (Alias for `cp`)
- `mv [source] [destination]` (Move a file)
- `rm [file]` (Remove a file)
- `touch [file]` (Create an empty file or update its timestamp)
- `pwd` (Print the current working directory)
- `df` (Disk usage)
- `free` (Show memory usage)
- `chmod [permissions] [file]` (Change file permissions)
- `chdir [directory]` (Change directory)
- `cd [directory]` (Alias for `chdir`)
- `cd..` (Move up one directory level)

#### User Management Commands:
- `relog [username]` (Logout and login with a specified username)
- `mkacct [username]` (Create a new user account)
- `userdel [username]` (Delete a user account)

#### Package Management Commands:
- `apt-get [command] [options] [package]` (Package management, custom implementation)
- `serve` (Host a file on IP and port)
- `download [url] [filename]` (Download a file from the internet)

#### Development and Utility Commands:
- `help` (Show help information)
- `print [message]` (Print a message to the terminal)
- `echo [message]` (Alias for `print`)
- `git [command] [options]` (Download from GitHub and save to a folder)

### Hyperion Features:

- **TFT Display Support**: Display API with interchangeable drivers.
- **Touch Screen Support**: Interface API for touch interaction.
- **Joystick Functionality**: Interface API for joystick control.
- **Button Functionality**: Interface API for buttons.
- **Beeper Functionality**: Interface API for the onboard beeper.
- **RGB LED Functionality**: Interface API for RGB LED control.
- **Module API (v0.1.0-beta)**: Interface for hardware modules integration.
- **Multi-threading (beta)**: Basic multi-threading support.
- **WiFi Functionality**: Networking support (Connect, Scan, Download, Serve, etc.)
- **Custom Package Manager**: For handling software packages and dependencies.
- **Morse Beeper Program**: Convert text to Morse code using the onboard beeper.
- **Serve Function**: Program to host files over IP and port.
- **Flash Memory File System**: FS API for file management.
- **Serial Support**: String input and output via serial interface.
- **Error Listing**: Utility for displaying system errors.
- **User Profiles**: Basic user management without encryption.
- **USB Keyboard Support**: Planned for future implementation.
- **Updating System**: Planned feature for updating the OS itself.
- **OS Verification**: Planned feature for file integrity checks using hash codes.
- **Security Enhancements**: Planned feature to lock system files and add a force delete option.
- **Sandboxing**: Planned feature for running untrusted code safely.
- **Debugging and Logging**: Python's native debugging with enhanced logging planned.

## Dev-kit Hardware Docs:
### Interfaces
Joystick(y = ADC1, x = ADC0)
Button-left(gp15)
Button-right(gp14)
Beeper(gp13)
Led-top(gp16)
Led-bottom(gp17)

### Display
- SCLK(gp2)
- MOSI(gp3)
- MISO(gp4)
- CS(gp5)
- DC(gp6)
- RST(gp7)
- LED(gp8)
- TPRST(gp10)
- TPINT(gp11)
- Touch-I2C0-SDA(gp8)
- Touch-I2C0-SCL(gp9)
