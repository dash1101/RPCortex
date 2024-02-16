![RPCortex](assets/RPCortex.png)

## Version: FUTURE PLANS!!!

RPCortex is a lightweight open-source operating system written in C++ for the RP2040 processor. It provides a platform for running user-written code, along with many built-in features to enhance functionality.

## Installation

Installing RPCortex is a simple, straightforward process. Follow these steps:

1. Download the latest release of RPCortex (`rpc[ver].uf2`) from the [Releases](https://github.com/DaSh1101/RPCortex/releases) page.
2. Extract the downloaded ZIP file to your local machine.
3. Connect your RP2040 device to your computer while holding the (`boot sel`) button.
4. Drag and drop the (`rpc[ver].uf2`) file to the root directory of your Raspberry Pi Pico W.
5. Your device should automatically reboot into RPCortex.

## Features

### Terminal CLI
- RPCortex currently is a CLI, but in the future, I plan to add a GUI with a desktop window manager, similar to some linux repos
 
### User-Written Code
- Execute custom code on your RP2040 using RPCortex.
- Currently supported languages: C++, html (for serving only)
  
### File Management
- Modify files and directories effortlessly.
- Rename and move files with ease.
- Read files, and launch programs through the terminal.
  
### Networking
- Pre-loaded networking drivers for Pico W.
- Download files directly from the internet.
- Host local files and public sites (requires port forwarding).
- Toggle to scan through a list of saved networks and auto-connect.

### Display Support
- Custom-made drivers to support ST7796 displays. TOUCH IS NOT SUPPORTED CURRENTLY!
- Support for communication thru serial at 9600 baud
- HDMI Support coming eventually...

## Getting Started

To get started with RPCortex, follow the installation steps mentioned below. Once installed, explore the built-in features and customize your experience.
- Upload (`rpc[ver].uf2`) to rp2040 device
- Get connected via [ST7796 display]([https://github.com/DaSh1101/RPCortex/?](http://www.lcdwiki.com/3.5inch_IPS_SPI_Module_ST7796)), Serial(`9600 baud`), and HDMI coming one day.
- SD SUPPORT FOR THE ST7796 IS NOT ACTIVE AS OF NOW, should be in the future though.
- Follow the setup process
- Enjoy!

## Usage

### File Management
Use the built-in file manager to navigate, modify, and organize your files.

- `write "Filename"` Creates a new file with the given name, then allows you to write files contents
- `mkdir "Directory Name"` Creates a new directory with the given name
- `dir` Lists files in a given directory
- `ls` Lists files in a given directory
- `cd "Directory"` Changes current working directory
- `chdir "Directory"` changes current working directory
- `edit "Filename"` Reads a file, then opens it to make changes / overwrite
- `open "Filename"` Reads a file, and opens/runs code when given the arg -s, ex: `open -s "main.cpp"`
- - `cat "Filename` same as `open`

### Networking
Configure networking settings and take advantage of the pre-loaded capabilities.

- `download url://url.url "Filename"` Attempts to download file from given url
- `curl url://url.url "Filename"` Same as download
- `wget url://url.url "Filename"` Same as download
- `serve 'IP:Port' 'Filename.HTML'`
- `net connect` Starts WiFi connection process
- `net disconnect` Disconnects
- `net scan` Scans for nearby WiFi connections
- `net autoconnect` Automatically connects to nearby network connections that have been previously connected to

### ST7796 Display
The pinout for the ST7796 display is the following:
| PIN | PIN  |
| GP2 | SCLK |
| GP3 | MOSI |
| GP4 | MISO |
| GP5 | CS   |
| GP6 | DC   |
| GP7 | RST  |

### Contributing
If you'd like to contribute to the development of RPCortex, follow these steps:

1. Fork the repository.
2. Create a new branch for your feature or bug fix.
3. Make your changes and submit a pull request.

### License
RPCortex is licensed under the MIT License.

### Release Notes
**Version 0.1.0a**
