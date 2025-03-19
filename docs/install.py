# install.py - Installation script for RPCortex
import os
import time
import gc

print("Starting RPCortex Installation...")
print("--------------------------------")

# Display system information
try:
    import machine
    print(f"Device: {machine.unique_id()}")
    print(f"Frequency: {machine.freq() / 1000000}MHz")
except:
    print("Could not retrieve device information")

# Free up memory
gc.collect()
print(f"Memory available: {gc.mem_free() / 1024:.2f}KB")

# Check for required files
required_files = [
    "boot.py", 
    "main.py"
]

print("\nChecking for required files...")
existing_files = os.listdir()
for file in required_files:
    if file in existing_files:
        print(f"✓ {file} exists")
    else:
        print(f"✗ {file} missing")

# Create basic configuration
print("\nCreating configuration...")
try:
    with open('config.json', 'w') as f:
        f.write('{"device_name": "RPCortex", "version": "0.1", "initialized": true}')
    print("✓ Configuration created")
except Exception as e:
    print(f"✗ Failed to create configuration: {e}")

# Print installation summary
print("\nInstallation complete!")
print("Run 'import main' to start the application")
print("--------------------------------")
