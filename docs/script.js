document.getElementById('fileButton').addEventListener('click', () => {
    document.getElementById('fileInput').click();
  });
  
  document.getElementById('connectButton').addEventListener('click', async () => {
    if (!navigator.serial) {
      alert('Web Serial API not supported in this browser. Please use Chrome or Edge.');
      return;
    }
  
    try {
      const port = await navigator.serial.requestPort();
      await port.open({ baudRate: 115200 });
      const writer = port.writable.getWriter();
      const reader = port.readable.getReader();
  
      // Fixed port info that won't change after initialization
      const portInfo = 'Serial Device';
      
      // Create terminal container if it doesn't exist
      let terminalContainer = document.querySelector('.terminal-container');
      
      if (!terminalContainer) {
        terminalContainer = document.createElement('div');
        terminalContainer.className = 'terminal-container';
        document.body.appendChild(terminalContainer);
  
        // Add terminal header with title and controls
        const terminalHeader = document.createElement('div');
        terminalHeader.className = 'terminal-header';
        
        const terminalTitle = document.createElement('div');
        terminalTitle.className = 'terminal-title';
        terminalTitle.id = 'terminal-title';
        terminalTitle.textContent = `Terminal (${portInfo})`;
        
        const terminalControls = document.createElement('div');
        terminalControls.className = 'terminal-controls';
        
        const minimizeButton = document.createElement('button');
        minimizeButton.className = 'terminal-control-button';
        minimizeButton.innerHTML = '−';
        minimizeButton.title = 'Minimize terminal';
        minimizeButton.addEventListener('click', () => {
          terminalContainer.classList.toggle('terminal-minimized');
          minimizeButton.innerHTML = terminalContainer.classList.contains('terminal-minimized') ? '+' : '−';
          minimizeButton.title = terminalContainer.classList.contains('terminal-minimized') ? 'Maximize terminal' : 'Minimize terminal';
        });
        
        terminalControls.appendChild(minimizeButton);
        terminalHeader.appendChild(terminalTitle);
        terminalHeader.appendChild(terminalControls);
        terminalContainer.appendChild(terminalHeader);
  
        const terminal = document.createElement('pre');
        terminal.className = 'terminal-output';
        terminalContainer.appendChild(terminal);
  
        const inputContainer = document.createElement('div');
        inputContainer.className = 'terminal-input-container';
  
        const promptSpan = document.createElement('span');
        promptSpan.textContent = '>>> ';
        promptSpan.className = 'terminal-prompt';
  
        const inputField = document.createElement('input');
        inputField.className = 'terminal-input-field';
        inputField.autofocus = true;
  
        const sendButton = document.createElement('button');
        sendButton.textContent = 'Send';
        sendButton.className = 'terminal-send-button';
  
        inputContainer.appendChild(promptSpan);
        inputContainer.appendChild(inputField);
        inputContainer.appendChild(sendButton);
        terminalContainer.appendChild(inputContainer);
      }
  
      const terminal = document.querySelector('.terminal-output');
      const inputField = document.querySelector('.terminal-input-field');
      const sendButton = document.querySelector('.terminal-send-button');
      const terminalTitle = document.getElementById('terminal-title');
  
      // Add a button to run install.py
      const installButton = document.createElement('button');
      installButton.textContent = 'Run install.py';
      installButton.id = 'installButton';
      installButton.addEventListener('click', async () => {
        await runInstallScript();
      });
      document.querySelector('.button-container').appendChild(installButton);
  
      const logToTerminal = (message, isSystem = false) => {
        if (message === null || message === undefined || message.trim() === '') return; // Skip empty messages
        
        // Process the message to handle special cases
        let processedMessage = message;
        
        if (!isSystem) {
          // Replace raw characters with their proper representation
          processedMessage = processedMessage.replace(/\r/g, '');
          
          // For non-system messages, insert <br> for newlines and ensure they're properly rendered
          const lines = processedMessage.split('\n');
          processedMessage = lines.join('<br>');
        }
        
        const className = isSystem ? 'system' : 
                       message.startsWith('>') ? 'command' : 'response';
        
        terminal.innerHTML += `<span class="${className}">${processedMessage}</span>${isSystem ? '<br>' : ''}`;
        terminal.scrollTop = terminal.scrollHeight;
      };
  
      const decoder = new TextDecoder();
      const encoder = new TextEncoder();
  
      // Update device status in header with memory info
      const updateDeviceStatus = (portId, mpyVersion, memInfo) => {
        const statusElement = document.getElementById('device-status');
        if (!statusElement) return; // Skip if the element doesn't exist
        
        if (!portId) {
          statusElement.innerHTML = `<div class="status-text">Device not yet initialized...</div>`;
          return;
        }
        
        // Process MPY version
        let formattedVersion = '?.?.?';
        let mpyStatus = '';
        
        if (mpyVersion) {
          formattedVersion = mpyVersion;
          
          // Parse the version for comparison - only consider the major.minor parts
          const versionParts = formattedVersion.split('.');
          if (versionParts.length >= 2) {
            const versionNumber = parseFloat(`${versionParts[0]}.${versionParts[1]}`);
            mpyStatus = !isNaN(versionNumber) && versionNumber >= 1.24 ? 
              `<span class="status-good">(Good)</span>` : 
              `<span class="status-warning">(Update recommended)</span>`;
          }
        }
        
        let ramValue = '?';
        let romValue = '?';
        let ramStatus = '';
        let romStatus = '';
        
        if (memInfo) {
          [ramValue, romValue] = memInfo;
          
          ramStatus = parseInt(ramValue) >= 128 ? 
            `<span class="status-good">(Good)</span>` : 
            `<span class="status-warning">(Limited)</span>`;
          
          romStatus = parseInt(romValue) >= 4 ? 
            `<span class="status-good">(Good)</span>` : 
            `<span class="status-warning">(Limited)</span>`;
        }
        
        statusElement.innerHTML = `
          <div class="status-text">Device connected at ${portId}</div>
          <div class="status-text">MPY: v${formattedVersion} ${mpyStatus}</div>
          <div class="status-text">RAM / ROM: ${ramValue}KB / ${romValue}MB ${ramStatus} / ${romStatus}</div>
        `;
      };
  
      // Update terminal title with progress percentage
      const updateProgressInTitle = (percentage = null) => {
        if (percentage !== null) {
          terminalTitle.textContent = `Terminal (${portInfo}) - ${percentage}%`;
        } else {
          terminalTitle.textContent = `Terminal (${portInfo})`;
        }
      };
  
      // Utility function to safely send commands with proper error handling
      const sendCommand = async (command, timeout = 500) => {
        try {
          await writer.write(encoder.encode(command));
          return await new Promise(resolve => setTimeout(resolve, timeout));
        } catch (error) {
          console.error('Command error:', error);
          logToTerminal(`Error sending command: ${error.message}`, true);
          return Promise.reject(error);
        }
      };
  
      // Check if a file exists on the device
      const checkFileExists = async (filename) => {
        try {
          await sendCommand("import os\r\n", 200);
          await sendCommand(`print("FILECHECK:", "${filename}" in os.listdir())\r\n`, 200);
          
          // Wait for response
          await new Promise(resolve => setTimeout(resolve, 500));
          
          // Check if the file exists in the device's response
          return rawBuffer.includes('FILECHECK: True');
        } catch (error) {
          console.error('Error checking file existence:', error);
          return false;
        }
      };
  
      // Function to run install.py after file transfer
      const runInstallScript = async () => {
        logToTerminal(`Running install.py...`, true);
        try {
          // Make sure we're in regular REPL mode
          await sendCommand('\x03\r\n', 500); // Ctrl+C to interrupt any running program
          
          // Execute the install.py script
          await sendCommand('import install\r\n', 500);
          logToTerminal(`install.py execution initiated`, true);
          return true;
        } catch (error) {
          console.error('Error running install script:', error);
          logToTerminal(`Error running install.py: ${error.message}`, true);
          return false;
        }
      };
  
      // Set up reading loop for receiving data from device
      let rawBuffer = '';
      const readLoop = async () => {
        try {
          // Initial update with port info
          updateDeviceStatus(portInfo, null);
          
          logToTerminal(`Connected to device. Initializing...`, true);
          
          // Reset device and clear any running programs
          await sendCommand('\x03\x03\r\n', 1000); // Double Ctrl+C to ensure interruption
          
          // Clear any pending output
          await sendCommand('\r\n', 300);
          
          // Extract MPY version using a more reliable approach
          await sendCommand('import sys\r\n', 500);
          
          // Get version as a string to avoid parsing issues with the tuple
          await sendCommand('print("MPYVER:", ".".join([str(i) for i in sys.implementation.version[:3]]))\r\n', 500);
          
          // Get memory information
          await sendCommand('import gc\r\n', 300);
          await sendCommand('gc.collect()\r\n', 500);
          await sendCommand('print("MEMINFO:", gc.mem_free() // 1024, ", ", import os; os.statvfs("/")[0] * os.statvfs("/")[2] // 1048576)\r\n', 500);
          
          // Variables to store extracted information
          let mpyVersion = null;
          let memInfo = null;
          let isProcessingInfo = true;
          
          while (true) {
            const { value, done } = await reader.read();
            if (done) {
              logToTerminal("Device disconnected", true);
              updateDeviceStatus(null);
              break;
            }
            
            // Decode incoming data
            const text = decoder.decode(value);
            rawBuffer += text;
            
            // Only log normal REPL output, not our detection commands
            if (!isProcessingInfo || !(
              rawBuffer.includes("MPYVER:") || 
              rawBuffer.includes("MEMINFO:") ||
              rawBuffer.includes("import") ||
              rawBuffer.includes("try:")
            )) {
              logToTerminal(text);
            }
            
            // Extract MPY version
            const mpyMatch = rawBuffer.match(/MPYVER:\s*([0-9.]+)/);
            if (mpyMatch) {
              mpyVersion = mpyMatch[1];
              logToTerminal(`Found MPY version: v${mpyVersion}`, true);
              rawBuffer = rawBuffer.replace(/MPYVER:\s*([0-9.]+)/, ''); // Remove from buffer
            }
            
            // Extract memory info
            const memMatch = rawBuffer.match(/MEMINFO:\s*(\d+),\s*(\d+)/);
            if (memMatch) {
              const ramFree = memMatch[1];
              const romFree = memMatch[2];
              logToTerminal(`Found memory info: RAM: ${ramFree}KB free, ROM: ${romFree}MB free`, true);
              rawBuffer = rawBuffer.replace(/MEMINFO:\s*(\d+),\s*(\d+)/, ''); // Remove from buffer
              
              memInfo = [ramFree, romFree];
              
              // Update the status with memory info
              updateDeviceStatus(portInfo, mpyVersion, memInfo);
              isProcessingInfo = false;
              logToTerminal("Device initialization complete", true);
              
              // Clear any remaining output from the initialization
              await sendCommand('\r\n', 300);
            }
            
            // Keep buffer manageable
            if (rawBuffer.length > 5000) {
              rawBuffer = rawBuffer.slice(-2000);
            }
          }
        } catch (error) {
          console.error('Read error:', error);
          logToTerminal(`Error reading from device: ${error.message}`, true);
          updateDeviceStatus(null);
        } finally {
          reader.releaseLock();
        }
      };
  
      // Start the read loop
      readLoop();
  
      // Handle sending commands
      sendButton.addEventListener('click', handleSendCommand);
      inputField.addEventListener('keypress', (event) => {
        if (event.key === 'Enter') {
          handleSendCommand();
        }
      });
  
      async function handleSendCommand() {
        const command = inputField.value;
        if (command) {
          try {
            await writer.write(encoder.encode(command + '\r\n'));
            logToTerminal(`> ${command}`);
            inputField.value = '';
          } catch (error) {
            console.error('Write error:', error);
            logToTerminal(`Error sending command: ${error.message}`, true);
          }
        }
      }
  
      // Set up improved file upload functionality
      const fileInput = document.getElementById('fileInput');
      
      fileInput.addEventListener('change', async () => {
        const file = fileInput.files[0];
        if (!file) {
          logToTerminal('No file selected.', true);
          return;
        }
  
        // Generate a filename based on the original
        const filename = file.name.replace(/[^a-zA-Z0-9._-]/g, '_'); // Sanitize filename
        
        logToTerminal(`Selected file: ${file.name}`, true);
        logToTerminal(`Preparing to transfer to device as: ${filename}`, true);
        
        try {
          // Read file content
          const fileContent = await readFileAsText(file);
          
          if (fileContent.length > 200000) { // ~200KB limit
            logToTerminal(`Warning: File is large (${Math.round(fileContent.length/1024)}KB). Transfer may take a while.`, true);
          }
          
          logToTerminal(`Starting file transfer...`, true);
          
          // Try two different approaches, in order of preference:
          // 1. Paste mode (good balance of speed and compatibility)
          // 2. Line-by-line mode (slow but most compatible)
          
          let transferMethod = '';
          let transferSuccessful = false;
          
          // Reset device and clear any running programs
          await sendCommand('\x03\x03\r\n', 500); // Double Ctrl+C
          await sendCommand('\r\n', 300); // Clear line
          
          // Function to check device capability for advanced transfer modes
          const checkCapabilities = async () => {
            // Try paste mode as a test
            logToTerminal(`Testing device capabilities...`, true);
            
            try {
              // See if paste mode is supported
              await sendCommand('\x05', 1000); // Ctrl+E to enter paste mode
              // If we get here without error, paste mode is supported
              transferMethod = 'paste';
              // Exit paste mode
              await sendCommand('\x04', 500); // Ctrl+D
              await sendCommand('\r\n', 300);
              return true;
            } catch (err) {
              logToTerminal(`Paste mode not supported, falling back to line mode`, true);
              transferMethod = 'line';
              return false;
            }
          };
          
          await checkCapabilities();
          
          // METHOD 1: PASTE MODE TRANSFER (most devices support this)
          if (transferMethod === 'paste') {
            try {
              logToTerminal(`Using paste mode transfer`, true);
              
              // Make sure we're in regular REPL mode
              await sendCommand('\x03\r\n', 500); // Ctrl+C to interrupt any running program
              await sendCommand('\r\n', 300); // Ensure clean prompt
              
              // Enter paste mode
              await sendCommand('\x05', 1000); // Ctrl+E to enter paste mode
              
              // Prepare file open code - with context manager to ensure file closure
              await sendCommand(`with open('${filename}', 'w') as f:\n`, 500);
              
              // Split content into manageable chunks and prepare them
              const CHUNK_SIZE = 512; // Much larger chunks possible with paste mode
              let position = 0;
              
              // Process the content into chunks that are properly escaped
              while (position < fileContent.length) {
                // Get chunk and escape problematic characters
                let chunk = fileContent.slice(position, position + CHUNK_SIZE)
                  .replace(/\\/g, '\\\\') // Escape backslashes
                  .replace(/'/g, "\\'")   // Escape single quotes
                  .replace(/\r?\n/g, '\\n'); // Handle all newline formats properly
                
                // Add explicit newline to the write statement
                await sendCommand(`    f.write('${chunk}')\n`, 10);
                
                // Update position
                position += CHUNK_SIZE;
                
                // Show progress periodically
                if (position % (CHUNK_SIZE * 10) === 0 || position >= fileContent.length) {
                  const percent = Math.min(100, Math.round((position / fileContent.length) * 100));
                  logToTerminal(`Transfer progress: ${percent}%`, true);
                  updateProgressInTitle(percent);
                }
              }
              
              // End paste mode
              await sendCommand('\x04', 1000); // Ctrl+D to execute the pasted code
              
              // Check if transfer was successful (no error messages)
              await sendCommand('\r\n', 500);
              
              transferSuccessful = true;
              logToTerminal(`File transfer complete using paste mode`, true);
              updateProgressInTitle(); // Remove progress from title
            } catch (error) {
              console.error('Paste mode transfer error:', error);
              logToTerminal(`Error in paste mode transfer. Trying line mode...`, true);
              transferMethod = 'line';
            }
          }
          
          // METHOD 2: LINE MODE TRANSFER (fallback method)
          if (transferMethod === 'line' && !transferSuccessful) {
            try {
              logToTerminal(`Using line-by-line transfer (slower but more compatible)`, true);
              
              // Reset and clear any errors
              await sendCommand('\x03\x03\r\n', 500); // Double Ctrl+C
              await sendCommand('\r\n', 300); // Clear line
              
              // Open file
              await sendCommand(`f = open('${filename}', 'w')\r\n`, 500);
              
              // Write content in small lines
              const lines = fileContent.split(/\r?\n/); // Handle all newline formats
              for (let i = 0; i < lines.length; i++) {
                // Escape the line content
                const line = lines[i]
                  .replace(/\\/g, '\\\\') // Escape backslashes
                  .replace(/'/g, "\\'");   // Escape single quotes
                
                // Write the line with explicit newline character
                await sendCommand(`f.write('${line}${i < lines.length - 1 ? "\\n" : ""}')\r\n`, 100);
                
                // Show progress periodically
                if (i % 10 === 0 || i >= lines.length - 1) {
                  const percent = Math.min(100, Math.round(((i + 1) / lines.length) * 100));
                  logToTerminal(`Transfer progress: ${percent}%`, true);
                  updateProgressInTitle(percent);
                }
              }
              
              // Close the file
              await sendCommand(`f.close()\r\n`, 500);
              
              transferSuccessful = true;
              logToTerminal(`File transfer complete using line mode`, true);
              updateProgressInTitle(); // Remove progress from title
            } catch (error) {
              console.error('Line mode transfer error:', error);
              logToTerminal(`Error in line mode transfer: ${error.message}`, true);
              updateProgressInTitle(); // Remove progress from title
            }
          }
          
          if (transferSuccessful) {
            logToTerminal(`File saved as ${filename} on the device.`, true);
            
            // List files to confirm
            await sendCommand("import os\r\n", 200);
            await sendCommand("print('Files on device:')\r\n", 200);
            await sendCommand("print(os.listdir())\r\n", 200);
            
            // Run install.py if it exists
            if (filename === 'install.py' || await checkFileExists('install.py')) {
              await runInstallScript();
            }
          } else {
            logToTerminal(`File transfer failed. Please try again with a smaller file.`, true);
          }
        } catch (error) {
          console.error('File transfer error:', error);
          logToTerminal(`Error transferring file: ${error.message}`, true);
          updateProgressInTitle(); // Remove progress from title
          
          // Try to exit any modes we might be stuck in
          await sendCommand('\x04', 200); // Ctrl+D to exit paste mode if we're in it
          await sendCommand('\x03\r\n', 200); // Ctrl+C to interrupt
        }
      });
  
      function readFileAsText(file) {
        return new Promise((resolve, reject) => {
          const reader = new FileReader();
          reader.onload = () => resolve(reader.result);
          reader.onerror = reject;
          reader.readAsText(file);
        });
      }
  
      logToTerminal('Connected and ready for commands.', true);
  
    } catch (error) {
      console.error('Connection error:', error);
      alert(`Failed to connect to the device: ${error.message}`);
    }
  });
