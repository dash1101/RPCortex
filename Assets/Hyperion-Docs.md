# Hyperion OS Documentation

## Software Documentation

### Hyperion Commands:

#### Networking Commands:
- `wlan` - Manage WLAN settings.
- `wlan connect` - Connect to a WLAN network.
- `wlan disconnect` - Disconnect from the current WLAN network.
- `wlan scan` - Scan for available WLAN networks.
- `wlan info` - Display WLAN information.
- `wlan status` - Show WLAN connection status.
- `wlan autoconnect` - Enable or disable automatic WLAN connection.
- `ping [hostname]` - Ping a specified hostname.
- `ifconfig` - Display network interface configuration.
- `dnslookup [hostname]` - Perform a DNS lookup for a hostname.
- `wget [options] [url]` - Retrieve files from the web.
- `serve` - Host a file on a specific IP and port.
- `download [url] [filename]` - Download a file from the internet.


#### System Commands:
- `temp` - Display the current temperature.
- `system` - Display general system information.
- `reboot` - Reboot the OS.
- `shutdown` - Enter a low-power state and turn off the display.
- `uptime` - Show the system's uptime.
- `sleep` - Enter sleep mode.
- `exit` - Exit the OS or safely crash it.
- `sysinfo` - Display detailed system information.
- `clear` - Clear the terminal screen.
- `cls` - Alias for `clear`.

#### File System Commands:
- `ls` - List files in the current directory.
- `dir` - Alias for `ls`.
- `ls -l` - Detailed file list view.
- `write [filename]` - Write to a file.
- `mkdir [path]` - Create a new directory.
- `rmdir [path]` - Remove an empty directory.
- `delete [file_name]` - Delete a file.
- `del [file_name]` - Alias for `delete`.
- `read [filename]` - Read and display a file.
- `open [filename]` - Open and display a file.
- `view [filename]` - View a file’s content.
- `cat [filename]` - Display the content of a file.
- `edit [filename]` - Edit a file.
- `run [file]` - Run a script or executable file.
- `exec [code]` - Execute inline code.
- `launch [file]` - Launch a program.
- `start [file]` - Alias for `launch`.
- `rename [old_name] [new_name]` - Rename a file.
- `ren [old_name] [new_name]` - Alias for `rename`.
- `move [filename] [destination]` - Move a file.
- `cp [source] [destination]` - Copy a file.
- `copy [source] [destination]` - Alias for `cp`.
- `mv [source] [destination]` - Move a file.
- `rm [file]` - Remove a file.
- `touch [file]` - Create an empty file or update its timestamp.
- `pwd` - Print the current working directory.
- `df` - Display disk usage.
- `free` - Show memory usage.
- `chmod [permissions] [file]` - Change file permissions.
- `chdir [directory]` - Change the directory.
- `cd [directory]` - Alias for `chdir`.
- `cd..` - Move up one directory level.

#### User Management Commands:
- `relog [username]` - Logout and login with a specified username.
- `mkacct [username]` - Create a new user account.
- `userdel [username]` - Delete a user account.

#### Package Management Commands:
- `apt-get [command] [options] [package]` - Manage software packages.

#### Development and Utility Commands:
- `help` - Show help information for commands.
- `print [message]` - Print a message to the terminal.
- `echo [message]` - Alias for `print`.

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
