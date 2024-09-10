# Hyperion OS Devmap

## Software Documentation

### Hyperion Commands:
✅ - Works normally
🟨 - Functions, some issues
❌ - Not working at all
🟦 - High Priority unfinished / Not working

#### Networking Commands:
- ✅ `wlan connect` - Connect to a WLAN network.
- ✅ `wlan disconnect` - Disconnect from the current WLAN network.
- ✅ `wlan scan` - Scan for available WLAN networks.
- ✅ `wlan autoconnect` - Enable or disable automatic WLAN connection.
- ✅ `ping [hostname]` - Ping a specified hostname.
- ✅ `ifconfig` - Display network interface configuration.
- ✅ `dnslookup [hostname]` - Perform a DNS lookup for a hostname.
- 🟨 `serve` - Host a file on a specific IP and port.
- 🟨 `download [url] [filename]` - Download a file from the internet.


#### System Commands:
- ✅ `reboot` - Reboot the OS.
- ✅ `uptime` - Show the system's uptime.
- ✅ `sleep` - Enter sleep mode.
- ✅ `sysinfo` - Display detailed system information.
- ✅ `clear` - Clear the terminal screen.
- ✅ `cls` - Alias for `clear`.
- 🟦 `meminfo` - Gives details on the system memory (RAM in bytes): Free, Used, Total
- ❌ `release` - Attempts to free up system memory
- 🟦 `validation` - Runs OS validation script
- ❌ `eval` - Runs performance/system evaluation, and returns a score.
- ❌ `neofetch` - Replication of neofetch program
  
#### File System Commands:
- ✅ `ls` - List files in the current directory. # Works
- ✅ `dir` - Alias for `ls`.
- 🟦 `write [filename]` - Write to a file.
- ✅ `mkdir [path]` - Create a new directory.
- ✅ `rmdir [path]` - Remove an empty directory.
- ✅ `delete [file_name]` - Delete a file.
- ✅ `del [file_name]` - Alias for `delete`.
- ✅ `read [filename]` - Read and display a file.
- ✅ `open [filename]` - Open and display a file.
- ✅ `view [filename]` - View a file’s content.
- ✅ `cat [filename]` - Display the content of a file.
- ❌ `edit [filename]` - Edit a file.
- ✅ `exec() [code]` - Runs code written.
- ✅ `rename [old_name] [new_name]` - Rename a file.
- ✅ `ren [old_name] [new_name]` - Alias for `rename`.
- 🟦 `move [filename] [destination]` - Move a file.
- ❌ `cp [source] [destination]` - Copy a file.
- ❌ `copy [source] [destination]` - Alias for `cp`.
- 🟦 `mv [source] [destination]` - Move a file.
- ✅ `rm [file]` - Remove a file.
- ❌ `touch [file]` - Create a file, and write text to it.
- ✅ `pwd` - Print the current working directory.
- ✅ `df` - Display disk usage.
- ❌ `free` - Show memory usage.
- ✅ `chdir [directory]` - Change the directory.
- ✅ `cd [directory]` - Alias for `chdir`.
- ✅ `cd..` - Move up one directory level.
- ❌ `deflate [file] [destination]` - Extracts tar files to a given directory.
- ❌ `compress [source] [file]` - Compresses a file/folder to a tar archive.

#### User Management Commands:
- ✅ `relog [username]` - Logout and login with a specified username.
- ✅ `mkacct [username]` - Create a new user account.
- ✅ `userdel [username]` - Delete a user account.

#### Package Management Commands:
- 🟦 `pkg [options] [package]` - Manage software packages.

#### Development and Utility Commands:
- ✅ `help` - Show help information for commands.
- ✅ `print [message]` - Print a message to the terminal.
- ✅ `echo [message]` - Alias for `print`.

### Hyperion Features:

- ✅ **TFT Display Support**: Display API with interchangeable drivers.
- ✅ **Joystick Functionality**: Interface API for joystick control.
- ✅ **Button Functionality**: Interface API for buttons.
- ✅ **Beeper Functionality**: Interface API for the onboard beeper.
- ✅ **WiFi Functionality**: Networking support (Connect, Scan, Download, Serve, etc.)
- 🟦 **Package Manager**: For handling software packages and dependencies.
- ✅ **Serve Function**: Program to host files over IP and port.
- ✅ **Flash Memory File System**: FS API for file management.
- ✅ **Serial Support**: String input and output via serial interface.
- ✅ **User Profiles**: Basic user management system.
- 🟦 **OS Verification**: Planned feature for file integrity checks using hash codes.
- ✅ **System Redundancy**: System files are locked so no accidental OS deletions.

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
