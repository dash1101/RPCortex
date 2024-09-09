# Hyperion OS Devmap

## Software Documentation

### Hyperion Commands:

#### Networking Commands:
- `wlan connect` - Connect to a WLAN network. # Works
- `wlan disconnect` - Disconnect from the current WLAN network. # Works
- `wlan scan` - Scan for available WLAN networks. # Works
- `wlan autoconnect` - Enable or disable automatic WLAN connection. # Works
- `ping [hostname]` - Ping a specified hostname. # Works
- `ifconfig` - Display network interface configuration. # Works
- `dnslookup [hostname]` - Perform a DNS lookup for a hostname. # Works
- `serve` - Host a file on a specific IP and port. # Sorta Works
- `download [url] [filename]` - Download a file from the internet. # Not Functional


#### System Commands:
- `reboot` - Reboot the OS. # Works
- `uptime` - Show the system's uptime. # Works
- `sleep` - Enter sleep mode. # Works
- `sysinfo` - Display detailed system information. # Works
- `clear` - Clear the terminal screen. # Works
- `cls` - Alias for `clear`. # Works
- `neofetch` - Replication of neofetch program # Not Functional
#### File System Commands:
- `ls` - List files in the current directory. # Works
- `dir` - Alias for `ls`. # Works
- `write [filename]` - Write to a file. # Was working, not now
- `mkdir [path]` - Create a new directory. # Works
- `rmdir [path]` - Remove an empty directory. # Works
- `delete [file_name]` - Delete a file. # Works
- `del [file_name]` - Alias for `delete`. # Works
- `read [filename]` - Read and display a file. # Works
- `open [filename]` - Open and display a file. # Works
- `view [filename]` - View a file’s content. # Works
- `cat [filename]` - Display the content of a file. # Works
- `edit [filename]` - Edit a file. # Was working, not now
- `rename [old_name] [new_name]` - Rename a file. # Works
- `ren [old_name] [new_name]` - Alias for `rename`. # Works
- `move [filename] [destination]` - Move a file. # Was working, not now
- `cp [source] [destination]` - Copy a file. # Was working, not now
- `copy [source] [destination]` - Alias for `cp`. # Was working, not now
- `mv [source] [destination]` - Move a file. # Was working, not now
- `rm [file]` - Remove a file. # Works
- `touch [file]` - Create an empty file or update its timestamp. # Was working, not now
- `pwd` - Print the current working directory. # Works
- `df` - Display disk usage. # Works
- `free` - Show memory usage. # Was working, not now
- `chdir [directory]` - Change the directory. # Works
- `cd [directory]` - Alias for `chdir`. # Works
- `cd..` - Move up one directory level. # Works

#### User Management Commands:
- `relog [username]` - Logout and login with a specified username. # Works
- `mkacct [username]` - Create a new user account. # Works
- `userdel [username]` - Delete a user account. # Works

#### Package Management Commands:
- `pkg [command] [options] [package]` - Manage software packages.  # Not Functional

#### Development and Utility Commands:
- `help` - Show help information for commands. # Works
- `print [message]` - Print a message to the terminal. # Works
- `echo [message]` - Alias for `print`. # Works

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

## Dev-Kit Hardware Documentation

### Interfaces
- **Joystick**: 
  - Y-axis: `ADC1`
  - X-axis: `ADC0`
- **Buttons**: 
  - Left Button: `GP15`
  - Right Button: `GP14`
- **Beeper**: `GP13`
- **LEDs**: 
  - Top LED: `GP16`
  - Bottom LED: `GP17`

### Display
- **TFT Screen Connections**:
  - SCLK: `GP2`
  - MOSI: `GP3`
  - MISO: `GP4`
  - CS: `GP5`
  - DC: `GP6`
  - RST: `GP7`
  - LED: `GP8`
- **Touch Panel Connections**:
  - TPRST: `GP10`
  - TPINT: `GP11`
  - Touch I2C0 SDA: `GP8`
  - Touch I2C0 SCL: `GP9`

---
