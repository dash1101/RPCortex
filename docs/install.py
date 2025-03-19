# install.py - Sample installation script for ESP32
import os
import gc
import time

# Print a welcome message
print("Starting RPCortex installation...")
print("-----------------------------------")

# Show available memory
gc.collect()
free_mem = gc.mem_free() // 1024
print(f"Available memory: {free_mem}KB")

# List files in the current directory
print("\nFiles on the device:")
files = os.listdir()
for file in files:
    try:
        stats = os.stat(file)
        size = stats[6]  # Size in bytes
        print(f"  - {file} ({size} bytes)")
    except:
        print(f"  - {file} (unknown size)")

# Helper function to create directories safely
def create_dir_if_not_exists(path):
    try:
        if path not in os.listdir():
            os.mkdir(path)
            print(f"Created directory: {path}")
        else:
            print(f"Directory already exists: {path}")
    except Exception as e:
        print(f"Error creating directory {path}: {e}")

# Create application directories
print("\nSetting up application structure...")
create_dir_if_not_exists("app")
create_dir_if_not_exists("config")
create_dir_if_not_exists("data")

# Write a simple boot.py file if it doesn't exist
if "boot.py" not in os.listdir():
    try:
        with open("boot.py", "w") as f:
            f.write("""# boot.py - RPCortex boot configuration
import gc
import time
import network
import webrepl

# Enable garbage collection
gc.enable()

# Wait for network to initialize
time.sleep(1)

# Setup WLAN as access point
ap = network.WLAN(network.AP_IF)
ap.active(True)
ap.config(essid='RPCortex', password='rpcortex123')

# Start WebREPL
webrepl.start()

print("RPCortex system initialized")
""")
        print("Created boot.py file")
    except Exception as e:
        print(f"Error creating boot.py: {e}")

# Write a simple main.py file if it doesn't exist
if "main.py" not in os.listdir():
    try:
        with open("main.py", "w") as f:
            f.write("""# main.py - RPCortex main application
import time
import machine
import gc

# Set up LED pin
led = machine.Pin(2, machine.Pin.OUT)

# Simple blink function
def blink(times=3, delay=0.2):
    for _ in range(times):
        led.value(1)
        time.sleep(delay)
        led.value(0)
        time.sleep(delay)

print("RPCortex application starting...")
blink(5)

# Main loop
while True:
    gc.collect()
    # Heartbeat LED blink
    led.value(1)
    time.sleep(0.1)
    led.value(0)
    time.sleep(5)
""")
        print("Created main.py file")
    except Exception as e:
        print(f"Error creating main.py: {e}")

print("\nInstallation completed successfully!")
print("You can now reset the device to start the application.")
blink_times = 3
print(f"Blinking LED {blink_times} times to indicate success...")

# Simulate LED blinking in the terminal
for i in range(blink_times):
    print("LED ON")
    time.sleep(0.2)
    print("LED OFF")
    time.sleep(0.2)

print("Installation process complete.")
