# Fusion OS Software Documentation

## Networking Commands
- **`wlan connect`**: Connect to a WLAN network.
- **`wlan disconnect`**: Disconnect from the current WLAN network.
- **`wlan scan`**: Scan for available WLAN networks.
- **`wlan autoconnect`**: Enable or disable automatic WLAN connection.
- **`wlan`**: Shows help and info on wlan.
- **`ping [hostname]`**: Ping a specified hostname.
- **`ifconfig`**: Display network interface configuration.
- **`dnslookup [hostname]`**: Perform a DNS lookup for a hostname.
- **`serve [filename] [ip] [port]`**: Host a file on a specific IP and port.
- **`get [url] [path]`**: Download a file from the internet, and save it to a path.

## System Commands
- **`reboot`**: Reboot the OS.
- **`uptime`**: Show the system's uptime.
- **`sysinfo`**: Display detailed system information.
- **`clear`**: Clear the terminal screen.
- **`cls`**: Alias for `clear`.
- **`meminfo`**: Display system memory details (free, used, total in bytes).
- **`clean`**: Free up system memory.
- **`validation`**: Run the OS validation script.
- **`pulse [oc]`**: Overclocks processor speed.
- **`pulse [uc]`**: Underclocks processor speed.
- **`bench`**: Benchmarks hardware speed.

## File System Commands
- **`ls`**: List files in the current directory.
- **`dir`**: Alias for `ls`.
- **`write [filename]`**: Write to a file.
- **`mkdir [path]`**: Create a new directory.
- **`rmdir [path]`**: Remove an empty directory.
- **`delete [filename]`**: Delete a file.
- **`del [filename]`**: Alias for `delete`.
- **`rm [filename]`**: Alias for `delete`.
- **`read [filename]`**: Alias for `view`.
- **`open [filename]`**: Opens a .py file, or acts as an alias for `view`.
- **`view [filename]`**: View a file’s content.
- **`edit [filename]`**: Edit a file.
- **`exec [code]`**: Run specified code.
- **`rename [filename] [new_name]`**: Rename a file.
- **`ren [filename] [new_name]`**: Alias for `rename`.
- **`move [filename] [destination]`**: Move a file.
- **`copy [source] [destination]`**: Copy a file.
- **`cp [source] [destination]`**: Alias for `copy`.
- **`move [source] [destination]`**: Move a file.
- **`mv [source] [destination]`**: Alias for `move`.
- **`touch [file]`**: Create a file and write text to it.
- **`pwd`**: Print the current working directory.
- **`df`**: Display disk usage.
- **`chdir [directory]`**: Change the directory.
- **`cd [directory]`**: Alias for `chdir`.
- **`cd..`**: Move up one directory level.

## User Management Commands
- **`logout`**: Logs you out of the active aaccount.
- **`mkacct [username] [password]`**: Create a new user account.
- **`userdel [username] [password]`**: Delete a user account.
- **`chpswd [username] [pass_old] [pass_new]`**: Change the encrypted password of a user.

## Development and Utility Commands
- **`help`**: Show help information for commands.
- **`print [message]`**: Print a message to the terminal.
- **`echo [message]`**: Alias for `print`.
- **`unpack [PkgPath] [DestPath]`**: Unpack a package and extract its contents to the specified destination.
- **`pack [PkgDir] [PkgName] [Version] [ExecPath]`**: Create a package for installation. Example: `pack "/Users/root/sysinfo" "System Info" "1.0.0" "/Packages/System.Info/main.py"`.
- **`pkg [arg] [name/path]`**: More documentation on the package manager will be provided below. Example for offline: `pkg local "/Users/root/package.pkg"`.
- **`...`**: More commands coming soon for development.

## Package Manager

### Local Package Installation
- **Command:** `pkg local "/Users/root/package.pkg"`
  - **Description:** Installs the specified local package in the `/Packages/` directory using its name. For example, if the package is named "System Info," it will be located at `/Packages/System.Info`.

### Online Package Installation
- **Command:** `pkg online "System.Info"`
  - **Description:** Connects to an online server and scans the package repository for available packages, provided the device has network access.

### Package List Update
- **Command:** `pkg list-update`
  - **Description:** Updates the package lists that the system uses for available packages.

### Online Package Upgrade
- **Command:** `pkg upgrade online "System.Info"`
  - **Description:** Upgrades the specified package to the latest version available in the update list. If no package name is provided, all installed packages will be upgraded to their latest versions.

### Local Package Upgrade
- **Command:** `pkg upgrade local "System.Info" "/Users/root/system.info.pkg"`
  - **Description:** Upgrades a package using a local file. If an older version exists, it will be removed and replaced with the specified local `.pkg` file.

### Package Removal
- **Command:** `pkg remove "System.Info"`
  - **Description:** Removes the specified package (e.g., "System.Info") along with its user data if prompted.

### User Local Data
- **Note:** Developers can create programs that retain essential user data even after upgrades by storing files in the user's "app.data" folder, which is accessible via the RPCortex API.

## Symbols
- [:] Info
- [!] Error
- [!!!] Fatal Error
- [?] Warn
- [@] Ok / Success
- ••> Input

More documentation will be available soon as the package manager continues to develop.
