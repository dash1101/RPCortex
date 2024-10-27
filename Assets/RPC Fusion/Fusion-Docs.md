# Fusion Software Documentation
*Written by dash1101*

---

## Fusion Commands

Fusion OS is still in early development, so it’s tough to say exactly what all the commands will be. For now, you can assume most commands from Hyperion will work similarly, but expect things to change as Fusion evolves.

---

## Error Codes

Same story as with the commands—error codes aren’t fully set up yet. I’ll add more details here as they get implemented.

---

## Startup Codes / `Settings.Note`

The `Settings.Startup` setting in Fusion OS lets you know what’s happening during startup. Here’s a breakdown of each possible value:

- **Settings.Startup = 0**: System is ready for a normal boot.
- **Settings.Startup = 1**: Something went wrong, so the system has entered recovery mode.
- **Settings.Startup = 2**: System is unstable or missing critical files.
- **Settings.Startup = 3**: System update failed.
- **Settings.Startup = 4**: System update installed successfully; everything’s good to go!
- **Settings.Startup = 5**: System is booting into safe mode.
- **Settings.Startup = 6**: System failure due to a clock change; the processor does not support clock speed adjustments. 

---

## Pulse Software

**What is it?**  
Pulse is the hardware management script that keeps Fusion OS running smoothly on the hardware side. Here’s what it can do:

- **Overclock** or **Underclock** the system as needed.
- Run **full hardware diagnostics**, including system hardware checks and benchmarking.

With Pulse, Fusion can:

- **Benchmark system performance** to get an accurate read on speed and efficiency.
- **Test memory** to check its status and functionality.
- **Test processor** performance and health.
- **Verify flash storage** capacity and functionality.

---

## Recovery Mode

**What is it?**  
Recovery mode is a system within Fusion OS that lets RPC Fusion make necessary changes if issues arise. Here’s what you can do in recovery mode:

- **Reset Fusion** to its initial state.
- **Repair Fusion** by fixing corrupted files or configurations.
- **Flash the system** with a recovery file to restore functionality.
- **Export current system flash** as a backup file.
- **Remove users** (requires root password).
- **Create or remove files** (also requires root password).
- **Change system clock speeds**
- 
---

As Fusion continues to grow, I’ll keep this documentation updated with more commands, error codes, and other info.
