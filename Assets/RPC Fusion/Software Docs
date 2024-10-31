# Fusion OS Software Documentation

### Networking Commands:
- `wlan connect` - Connect to a WLAN network.
- `wlan disconnect` - Disconnect from the current WLAN network.
- `wlan scan` - Scan for available WLAN networks.
- `wlan autoconnect` - Enable or disable automatic WLAN connection.
- `ping [hostname]` - Ping a specified hostname.
- `ifconfig` - Display network interface configuration.
- `dnslookup [hostname]` - Perform a DNS lookup for a hostname.
- `serve` - Host a file on a specific IP and port.
- `download [url] [filename]` - Download a file from the internet.


### System Commands:
- `reboot` - Reboot the OS.
- `uptime` - Show the system's uptime.
- `sleep` - Enter sleep mode.
- `sysinfo` - Display detailed system information.
- `clear` - Clear the terminal screen.
- `cls` - Alias for `clear`.
- `meminfo` - Gives details on the system memory (RAM in bytes): Free, Used, Total.
- `clean` - Frees up system memory.
- `validation` - Runs OS validation script.
  
### File System Commands:
- `ls` - List files in the current directory. # Works
- `dir` - Alias for `ls`.
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
- `exec() [code]` - Runs code written.
- `rename [old_name] [new_name]` - Rename a file.
- `ren [old_name] [new_name]` - Alias for `rename`.
- `move [filename] [destination]` - Move a file.
- `cp [source] [destination]` - Copy a file.
- `copy [source] [destination]` - Alias for `cp`.
- `mv [source] [destination]` - Move a file.
- `rm [file]` - Remove a file.
- `touch [file]` - Create a file, and write text to it.
- `pwd` - Print the current working directory.
- `df` - Display disk usage.
- `chdir [directory]` - Change the directory.
- `cd [directory]` - Alias for `chdir`.
- `cd..` - Move up one directory level.

### User Management Commands:
- `relog [username]` - Logout and login with a specified username.
- `mkacct [username]` - Create a new user account.
- `userdel [username]` - Delete a user account.
- `pswd [username]` - Change the encrypted password of a user.

### Development and Utility Commands:
- `help` - Show help information for commands.
- `print [message]` - Print a message to the terminal.
- `echo [message]` - Alias for `print`.
- `unpack [PkgPath] [DestPath]` - Unpacks a package and extracts its contents to the `DestPath`.
- `pack [PkgDir] [PkgName] [Version] [ExecPath]` - Creates a package for the system to be able to install, ex: pack "/Users/root/sysinfo" "System Info" "1.0.0" "/Packages/System Info/main.py"
- `pkg [arg] [name/path]` - More documentation on the package manager will be listed below, here's an example for offline: pkg local "/Users/root/package.pkg"
- `...` - More commands coming soon for development

### Package Manager

#### Local Package Installation
- **Command:** `pkg local "/Users/root/package.pkg"`
  - **Description:** Installs the specified local package. The package will be installed in the `/Packages/` directory using its name. For example, if the package is named "System Info," it will be located at `/Packages/System.Info`.

#### Online Package Installation
- **Command:** `pkg online "System.Info"`
  - **Description:** Connects to an online server and scans the package repository for available packages, provided the device has network access.

#### Package List Update
- **Command:** `pkg list-update`
  - **Description:** Updates the package lists that the system uses for available packages.

#### Online Package Upgrade
- **Command:** `pkg upgrade online "System.Info"`
  - **Description:** Upgrades the specified package to the latest version available in the update list. If no package name is provided, all installed packages will be upgraded to their latest versions.

#### Local Package Upgrade
- **Command:** `pkg upgrade local "System.Info" "/Users/root/system.info.pkg"`
  - **Description:** Upgrades a package using a local file. If an older version exists, it will be removed and replaced with the specified local `.pkg` file.

#### Package Removal
- **Command:** `pkg remove "System.Info"`
  - **Description:** Removes the specified package (e.g., "System.Info") along with its user data if prompted.

#### User Local Data
- **Note:** Developers can create programs that retain essential user data even after upgrades by storing files in the user's "app.data" folder, which is accessible via the RPCortex API.

More documentation will be available soon as the package manager continues to develop.
---
