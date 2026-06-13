# RPCortex Launchpad - installed program commands
# Format: command_name:/path/to/file.py:function_name
# Add entries here when packages register shell commands.
# Built-in entries below are self-healed by launchpad.load_commands() if missing.

pkg:/Core/Launchpad/pkg.py:pkg
wifi:/Core/Launchpad/wifi.py:wifi
# fetch/neofetch/bench/ntp are added by load_commands()'s self-heal ONLY when
# their package files are actually present (.py OR .mpy) — so this file never
# hardcodes a command for a package that may not be shipped/installed.
