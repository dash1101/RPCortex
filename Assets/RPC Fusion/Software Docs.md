# Fusion OS Basic Software Documentation

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

## Misc
- **`help`**: Show help information for commands.
- **`print [message]`**: Print a message to the terminal.
- **`echo [message]`**: Alias for `print`.

### User Local Data
- **Note:** Developers can create programs that retain essential user data even after upgrades by storing files in the user's "app.data" folder, which is accessible via the RPCortex API.

## Symbols
- #### $\textcolor{purple}{[}\textcolor{white}{:}\textcolor{purple}{]}$ Info
- #### $\textcolor{red}{[}\textcolor{white}{!}\textcolor{red}{]}$  Error
- #### $\textcolor{red}{[}\textcolor{white}{!!!}\textcolor{red}{]}$ Fatal Error
- #### $\textcolor{yellow}{[}\textcolor{white}{?}\textcolor{yellow}{]}$ Warn
- #### $\textcolor{aqua}{[}\textcolor{white}{@}\textcolor{aqua}{]}$ Ok / Success
- #### $\textcolor{aqua}{••>}$ Input
More documentation will be available soon as the package manager continues to develop.
