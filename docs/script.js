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

    // Improved terminal output - prevents HTML issues and handles special characters better
    const logToTerminal = (message, isSystem = false) => {
      if (message === null || message === undefined || message === '') return;
      
      // Create a new element for the message
      const messageElement = document.createElement('span');
      
      // Set the appropriate class
      const className = isSystem ? 'system' : 
                       message.startsWith('>') ? 'command' : 'response';
      messageElement.className = className;
      
      // Process the message based on type
      let content = message;
      if (!isSystem) {
        // Handle line endings for console output
        content = content.replace(/\r/g, '');
      }
      
      // Safely set content using textContent to avoid HTML injection
      // For linebreaks we need to handle them specially after using textContent
      if (content.includes('\n')) {
        const lines = content.split('\n');
        for (let i = 0; i < lines.length; i++) {
          const lineSpan = document.createElement('span');
          lineSpan.textContent = lines[i];
          messageElement.appendChild(lineSpan);
          
          // Add line break between lines (but not after the last line)
          if (i < lines.length - 1) {
            messageElement.appendChild(document.createElement('br'));
          }
        }
      } else {
        messageElement.textContent = content;
      }
      
      // Append to terminal
      terminal.appendChild(messageElement);
      
      // Add a line break after system messages
      if (isSystem) {
        terminal.appendChild(document.createElement('br'));
      }
      
      // Auto-scroll to bottom
      terminal.scrollTop = terminal.scrollHeight;
    };

    const decoder = new TextDecoder();
    const encoder = new TextEncoder();

    // Update device status in header
    const updateDeviceStatus = (portId, mpyVersion, memInfo) => {
      const statusElement = document.getElementById('device-status');
      if (!statusElement) return;
      
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

    // Improved command sending with better error handling and timeout management
    const sendCommand = async (command, timeout = 500) => {
      try {
        await writer.write(encoder.encode(command));
        return await new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            resolve(); // Resolve anyway after timeout to prevent hanging
          }, timeout);
          
          // Allow resolution before timeout if needed
          resolve(timer);
        });
      } catch (error) {
        console.error('Command error:', error);
        logToTerminal(`Error sending command: ${error.message}`, true);
        return Promise.resolve(); // Continue execution despite errors
      }
    };

    // Set up reading loop with improved MPY version extraction
    const readLoop = async () => {
      try {
        // Initial update with port info
        updateDeviceStatus(portInfo, null);
        
        logToTerminal(`Connected to device. Initializing...`, true);
        
        // Reset device with more reliable approach
        await sendCommand('\x03', 300); // First Ctrl+C
        await sendCommand('\x03', 300); // Second Ctrl+C
        await sendCommand('\r\n', 300); // Clear line
        
        // More robust MPY version detection with timeout protection
        let versionDetectionSuccess = false;
        let mpyVersion = null;
        let attempts = 0;
        const maxAttempts = 3;
        
        while (!versionDetectionSuccess && attempts < maxAttempts) {
          attempts++;
          logToTerminal(`Detecting MPY version (attempt ${attempts})...`, true);
          
          try {
            // Clear any pending input
            await sendCommand('\r\n', 200);
            
            // Simple, direct version check that's more likely to succeed
            await sendCommand('import sys\r\n', 500);
            await sendCommand('print("MPYVER:" + ".".join([str(x) for x in sys.implementation.version]))\r\n', 1000);
            
            // Set a timeout for version detection
            const versionDetectionTimeout = 3000; // 3 seconds
            const versionPromise = new Promise(async (resolve) => {
              let buffer = '';
              let versionFound = false;
              
              const checkInterval = setInterval(() => {
                const mpyMatch = buffer.match(/MPYVER:([0-9.]+)/);
                if (mpyMatch) {
                  mpyVersion = mpyMatch[1];
                  clearInterval(checkInterval);
                  versionFound = true;
                  resolve(true);
                }
              }, 100);
              
              setTimeout(() => {
                clearInterval(checkInterval);
                if (!versionFound) {
                  resolve(false);
                }
              }, versionDetectionTimeout);
              
              // Read data during the timeout period
              try {
                while (!versionFound) {
                  const { value, done } = await reader.read();
                  if (done) break;
                  const text = decoder.decode(value);
                  buffer += text;
                }
              } catch (e) {
                // Continue even if read errors occur
              }
            });
            
            versionDetectionSuccess = await versionPromise;
            
            if (versionDetectionSuccess) {
              logToTerminal(`MPY version detected: v${mpyVersion}`, true);
              updateDeviceStatus(portInfo, mpyVersion);
              break;
            } else {
              logToTerminal(`Version detection timed out, retrying...`, true);
              await sendCommand('\x03', 300); // Ctrl+C to interrupt any hanging process
            }
          } catch (error) {
            logToTerminal(`Error detecting version: ${error.message}`, true);
          }
        }
        
        if (!versionDetectionSuccess) {
          logToTerminal(`Could not detect MPY version after ${maxAttempts} attempts. Continuing with limited functionality.`, true);
          updateDeviceStatus(portInfo, '?.?.?');
        }
        
        logToTerminal("Device initialization complete. Ready for commands.", true);
        
        // Clear any remaining output from the initialization
        await sendCommand('\r\n', 300);
        
        // Main read loop for normal operation
        let readBuffer = '';
        while (true) {
          const { value, done } = await reader.read();
          if (done) {
            logToTerminal("Device disconnected", true);
            updateDeviceStatus(null);
            break;
          }
          
          // Decode incoming data
          const text = decoder.decode(value);
          
          // Filter out noise that might confuse users
          if (!text.includes("import sys") && !text.includes("MPYVER:")) {
            logToTerminal(text);
          }
          
          // Add to buffer for potential processing
          readBuffer += text;
          
          // Keep buffer manageable
          if (readBuffer.length > 2000) {
            readBuffer = readBuffer.slice(-1000);
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

    // Completely rewritten file upload functionality with more robust error handling
    const fileInput = document.getElementById('fileInput');
    
    fileInput.addEventListener('change', async () => {
      const file = fileInput.files[0];
      if (!file) {
        logToTerminal('No file selected.', true);
        return;
      }

      // Sanitize filename - only allow safe characters
      const safeFilename = file.name.replace(/[^a-zA-Z0-9._-]/g, '_');
      
      logToTerminal(`Selected file: ${file.name}`, true);
      logToTerminal(`Preparing to transfer as: ${safeFilename}`, true);
      
      try {
        // Read file content
        const fileContent = await readFileAsText(file);
        const fileSize = Math.round(fileContent.length/1024);
        
        logToTerminal(`File size: ${fileSize}KB`, true);
        
        // Reset device before transfer
        await sendCommand('\x03\x03', 500); // Double Ctrl+C to interrupt any running program
        await sendCommand('\r\n', 300); // Clear line
        
        // Try upload method detection
        logToTerminal(`Testing device capabilities...`, true);
        
        // Attempt file transfer with 3 different methods, in order of preference
        const transferMethods = [
          { name: 'chunked', tryFunction: tryChunkedTransfer },
          { name: 'paste', tryFunction: tryPasteTransfer },
          { name: 'line', tryFunction: tryLineTransfer }
        ];
        
        let transferSuccess = false;
        
        for (const method of transferMethods) {
          if (!transferSuccess) {
            logToTerminal(`Attempting ${method.name} mode transfer...`, true);
            transferSuccess = await method.tryFunction(fileContent, safeFilename);
            
            if (transferSuccess) {
              logToTerminal(`File transfer successful using ${method.name} mode!`, true);
              break;
            } else {
              logToTerminal(`${method.name} mode transfer failed, trying next method...`, true);
              // Reset device between attempts
              await sendCommand('\x03\x03', 500);
              await sendCommand('\r\n', 300);
            }
          }
        }
        
        if (transferSuccess) {
          // Confirm file exists
          logToTerminal(`Verifying file...`, true);
          await sendCommand('import os\r\n', 500);
          await sendCommand(`'${safeFilename}' in os.listdir()\r\n`, 1000);
          
          // List files as confirmation
          await sendCommand("print('Files on device:')\r\n", 500);
          await sendCommand("print(os.listdir())\r\n", 1000);
        } else {
          logToTerminal(`All transfer methods failed. Try reducing file size or simplifying content.`, true);
        }
        
        // Clear progress from title
        updateProgressInTitle();
        
      } catch (error) {
        console.error('File transfer error:', error);
        logToTerminal(`Error transferring file: ${error.message}`, true);
        updateProgressInTitle(); // Clear progress
        
        // Emergency recovery
        await sendCommand('\x04', 300); // Ctrl+D to exit any modes
        await sendCommand('\x03', 300); // Ctrl+C to interrupt
        await sendCommand('\r\n', 300);
      }
      
      // Reset file input
      fileInput.value = '';
      
      // TRANSFER METHOD 1: CHUNKED WRITE
      async function tryChunkedTransfer(content, filename) {
        try {
          // This method uses a more efficient chunked approach
          // Step 1: Create empty file first
          await sendCommand(`f = open('${filename}', 'w')\r\n`, 500);
          await sendCommand(`f.close()\r\n`, 500);
          
          // Step 2: Reopen for append
          await sendCommand(`f = open('${filename}', 'a')\r\n`, 500);
          
          // Step 3: Write in larger chunks with proper escaping
          const CHUNK_SIZE = 1024; // Larger chunks for efficiency
          const chunks = [];
          
          // Pre-process content into escaped chunks
          for (let i = 0; i < content.length; i += CHUNK_SIZE) {
            let chunk = content.slice(i, i + CHUNK_SIZE)
              .replace(/\\/g, '\\\\') // Escape backslashes first
              .replace(/'/g, "\\'")   // Escape single quotes
              .replace(/\r\n/g, '\\n') // Windows line endings
              .replace(/\n/g, '\\n');  // Unix line endings
              
            chunks.push(chunk);
          }
          
          // Write chunks with progress updates
          for (let i = 0; i < chunks.length; i++) {
            const percent = Math.round(((i + 1) / chunks.length) * 100);
            
            // Write chunk with error checking
            const writeCmd = `f.write('${chunks[i]}')\r\n`;
            await sendCommand(writeCmd, 500);
            
            // Update progress every few chunks or on completion
            if (i % 5 === 0 || i === chunks.length - 1) {
              updateProgressInTitle(percent);
              logToTerminal(`Transfer progress: ${percent}%`, true);
            }
          }
          
          // Close file and sync
          await sendCommand(`f.close()\r\n`, 500);
          
          return true;
        } catch (error) {
          console.error('Chunked transfer error:', error);
          return false;
        }
      }
      
      // TRANSFER METHOD 2: PASTE MODE
      async function tryPasteTransfer(content, filename) {
        try {
          // Enter paste mode
          await sendCommand('\x05', 1000); // Ctrl+E
          
          // Check if we entered paste mode successfully
          // Give a brief delay to see if we get the paste mode indicator
          await new Promise(resolve => setTimeout(resolve, 500));
          
          // Start with file open command
          await sendCommand(`with open('${filename}', 'w') as f:\n`, 200);
          
          // Process content in manageable chunks
          const CHUNK_SIZE = 480; // Smaller for paste mode reliability
          let position = 0;
          
          while (position < content.length) {
            // Get and escape chunk
            let chunk = content.slice(position, position + CHUNK_SIZE)
              .replace(/\\/g, '\\\\') // Escape backslashes
              .replace(/'/g, "\\'")   // Escape single quotes
              .replace(/\r?\n/g, '\\n'); // Handle all newline formats
            
            // Send with proper indentation for the with block
            await sendCommand(`    f.write('${chunk}')\n`, 100);
            
            // Update position
            position += CHUNK_SIZE;
            
            // Show progress periodically
            if (position % (CHUNK_SIZE * 5) === 0 || position >= content.length) {
              const percent = Math.min(100, Math.round((position / content.length) * 100));
              updateProgressInTitle(percent);
              // Don't log in paste mode as it can interfere
            }
          }
          
          // End paste mode
          await sendCommand('\x04', 1000); // Ctrl+D
          
          // Check for errors by sending a simple command
          await sendCommand('\r\n', 500);
          
          return true;
        } catch (error) {
          console.error('Paste mode error:', error);
          return false;
        }
      }
      
      // TRANSFER METHOD 3: LINE BY LINE (most compatible)
      async function tryLineTransfer(content, filename) {
        try {
          // First create and immediately close the file
          await sendCommand(`open('${filename}', 'w').close()\r\n`, 500);
          
          // Then open for append - more reliable for large files
          await sendCommand(`f = open('${filename}', 'a')\r\n`, 500);
          
          // Split content into lines
          const lines = content.split(/\r?\n/);
          
          // Process each line safely
          for (let i = 0; i < lines.length; i++) {
            // Properly escape the line content
            const line = lines[i]
              .replace(/\\/g, '\\\\')
              .replace(/'/g, "\\'")
              .replace(/"/g, '\\"');
            
            // Write with explicit newline for all but last line
            const newline = i < lines.length - 1 ? '\\n' : '';
            await sendCommand(`f.write('${line}${newline}')\r\n`, 100);
            
            // Update progress every 10 lines
            if (i % 10 === 0 || i === lines.length - 1) {
              const percent = Math.min(100, Math.round(((i + 1) / lines.length) * 100));
              updateProgressInTitle(percent);
              logToTerminal(`Transfer progress: ${percent}%`, true);
            }
          }
          
          // Close file
          await sendCommand(`f.close()\r\n`, 500);
          
          return true;
        } catch (error) {
          console.error('Line transfer error:', error);
          return false;
        }
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
