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

    const logToTerminal = (message, isSystem = false) => {
      if (message === null || message === undefined || message.trim() === '') return; // Skip empty messages

      // Create a new span element for this message
      const span = document.createElement('span');

      // Determine the appropriate class
      const className = isSystem ? 'system' :
        message.startsWith('>') ? 'command' : 'response';
      span.className = className;

      // For system messages, just set the text directly
      if (isSystem) {
        span.textContent = message;
        terminal.appendChild(span);
        terminal.appendChild(document.createElement('br'));
      } else {
        // For normal output, we need to handle the text differently
        // Remove carriage returns but preserve newlines
        message = message.replace(/\r/g, '');

        // Split by newlines to add each line separately
        const lines = message.split('\n');
        for (let i = 0; i < lines.length; i++) {
          if (i > 0) {
            // Add a line break between lines
            terminal.appendChild(document.createElement('br'));
          }

          // Add the line text
          const lineSpan = document.createElement('span');
          lineSpan.className = className;
          lineSpan.textContent = lines[i];
          terminal.appendChild(lineSpan);
        }
      }

      // Scroll to the bottom
      terminal.scrollTop = terminal.scrollHeight;
    };

    const decoder = new TextDecoder();
    const encoder = new TextEncoder();

    // Update device status in header with simplified parsing
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

    // Improved utility function to send commands with proper error handling and response tracking
    const sendCommand = async (command, timeout = 500, waitForResponse = false) => {
      try {
        // Split multi-line commands and send them one at a time
        const lines = command.split('\n');

        if (lines.length > 1) {
          // For multi-line commands, send each line separately
          for (const line of lines) {
            if (line.trim()) {
              await writer.write(encoder.encode(line + '\r\n'));
              // Small delay between lines
              await new Promise(resolve => setTimeout(resolve, 100));
            }
          }
        } else {
          // Single line command
          await writer.write(encoder.encode(command));
        }

        if (waitForResponse) {
          // Return a promise that will be resolved by the read loop when matching response is found
          return new Promise(resolve => {
            // Store command in the pending commands queue
            pendingCommands.push({ command, resolver: resolve });
          });
        }

        return await new Promise(resolve => setTimeout(resolve, timeout));
      } catch (error) {
        console.error('Command error:', error);
        logToTerminal(`Error sending command: ${error.message}`, true);
        return Promise.reject(error);
      }
    };

    // Queue for tracking commands waiting for responses
    const pendingCommands = [];

    // Buffer for accumulating response data
    let responseBuffer = '';

    // Set up reading loop for receiving data from device
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

        // Variables to store extracted information
        let mpyVersion = null;
        let rawBuffer = '';
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
          responseBuffer += text;

          // Only log normal REPL output, not our detection commands
          if (!isProcessingInfo || !(
            rawBuffer.includes("MPYVER:") ||
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

            // Update the status with fixed port info
            updateDeviceStatus(portInfo, mpyVersion);
            isProcessingInfo = false;
            logToTerminal("Device initialization complete", true);

            // Clear any remaining output from the initialization
            await sendCommand('\r\n', 300);
          }

          // Check for pending command responses
          if (pendingCommands.length > 0 && responseBuffer.includes('>>>')) {
            // We have a complete response to process
            const command = pendingCommands.shift();
            if (command && command.resolver) {
              command.resolver(responseBuffer);
              responseBuffer = ''; // Clear the buffer after processing
            }
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

    // Helper function to detect device capabilities
    const checkDeviceCapabilities = async () => {
      try {
        // Check if we can use memory-efficient base64 decoding
        const testResult = await sendCommand('import binascii\r\n', 300);
        await sendCommand('import gc\r\n', 300);
        return { hasBase64: true };
      } catch (error) {
        return { hasBase64: false };
      }
    };

    // New function for chunked base64 file writing
    async function writeBase64ChunkedFile(filename, base64Data) {
      logToTerminal(`Starting base64 file transfer for ${filename}...`, true);

      try {
        // Generate a random number for the filename
        const randomNum = Math.floor(Math.random() * 10000);
        const actualFilename = filename.replace('{random-int}', randomNum);

        // Reset device state
        await sendCommand('\x03\x03\r\n', 1000); // Double Ctrl+C with longer timeout
        await sendCommand('\r\n', 500); // Clear line with longer timeout

        // Check capabilities
        const capabilities = await checkDeviceCapabilities();

        // Memory-efficient approach - process base64 in chunks
        const CHUNK_SIZE = 256; // Reduce chunk size for better reliability
        let position = 0;
        let fileOpened = false;

        // Initial file creation
        await sendCommand(`f = open('${actualFilename}', 'wb')\r\n`, 1000);
        fileOpened = true;

        // Process in chunks to avoid memory issues
        while (position < base64Data.length) {
          // Calculate actual chunk size to ensure we don't cut base64 padding
          const end = Math.min(position + CHUNK_SIZE, base64Data.length);
          const chunk = base64Data.substring(position, end);

          // Write the chunk
          if (capabilities.hasBase64) {
            // More efficient method
            await sendCommand(`f.write(binascii.a2b_base64('${chunk}'))\r\n`, 1000); // Longer timeout
            // Force garbage collection after each chunk to prevent memory issues
            await sendCommand('gc.collect()\r\n', 300);
          } else {
            // Fallback method
            await sendCommand(`f.write(bytes([int(x) for x in '${chunk}'.encode('utf-8')]))\r\n`, 1500);
          }

          // Update position and progress
          position = end;
          const percent = Math.min(100, Math.round((position / base64Data.length) * 100));

          if (position % (CHUNK_SIZE * 5) === 0 || position >= base64Data.length) {
            logToTerminal(`Transfer progress: ${percent}%`, true);
            updateProgressInTitle(percent);
          }

          // Add a small delay between chunks to prevent overloading the device
          await new Promise(resolve => setTimeout(resolve, 100));
        }

        // Add a delay before closing the file
        await new Promise(resolve => setTimeout(resolve, 500));

        // Close file
        await sendCommand('f.close()\r\n', 1000);

        // Additional delay before returning
        await new Promise(resolve => setTimeout(resolve, 1000));

        logToTerminal(`Base64 file transferred and saved as ${actualFilename}`, true);
        updateProgressInTitle();

        return actualFilename;
      } catch (error) {
        console.error('Base64 file transfer error:', error);
        logToTerminal(`Error in base64 file transfer: ${error.message}`, true);
        updateProgressInTitle();

        // Try to clean up if there was an error
        await sendCommand('\x03\r\n', 500); // Ctrl+C to interrupt any running process
        await sendCommand('try:\n    f.close()\nexcept:\n    pass\r\n', 500);

        return null;
      }
    }

    // New function to execute a file
    async function executeFile(filename) {
      try {
        logToTerminal(`Executing ${filename}...`, true);

        // Reset the device to ensure a clean state
        await sendCommand('\x03\x03\r\n', 1000); // Double Ctrl+C
        await new Promise(resolve => setTimeout(resolve, 500));

        // Clear any pending output
        await sendCommand('\r\n', 500);
        await new Promise(resolve => setTimeout(resolve, 500));

        // Use a one-liner command with try/except structure
        // This avoids the indentation issues with the REPL
        const execCommand = `try: exec(open('${filename}').read())\nexcept Exception as e: print('Execution error:', repr(e))\r\n`;
        await sendCommand(execCommand, 5000);

        // Wait for execution to complete
        await new Promise(resolve => setTimeout(resolve, 3000));

        logToTerminal(`Execution of ${filename} complete`, true);
        return true;
      } catch (error) {
        console.error('Execution error:', error);
        logToTerminal(`Error executing ${filename}: ${error.message}`, true);

        // Try alternate approach as a fallback
        try {
          logToTerminal(`Trying alternate execution method...`, true);
          await sendCommand('\x03\r\n', 500); // Ctrl+C to interrupt any running process
          await sendCommand('\r\n', 500); // Clear line

          // Execute using the most direct method
          await sendCommand(`exec(open('${filename}').read())\r\n`, 5000);

          // Wait for execution to complete
          await new Promise(resolve => setTimeout(resolve, 3000));

          logToTerminal(`Alternate execution complete`, true);
          return true;
        } catch (altError) {
          console.error('Alternate execution error:', altError);
          logToTerminal(`Error with alternate execution: ${altError.message}`, true);

          // Try to recover
          await sendCommand('\x03\r\n', 500); // Ctrl+C to interrupt
          await sendCommand('\r\n', 500); // Clear line

          return false;
        }
      }
    }

    // Function to fetch remote firmware - FIXED to properly handle content types and sanitize content
    async function fetchFirmware(url) {
      logToTerminal(`Fetching firmware from ${url}...`, true);

      try {
        const response = await fetch(url);

        if (!response.ok) {
          throw new Error(`Network response was not ok: ${response.status}`);
        }

        // Get content type to determine how to process the response
        const contentType = response.headers.get('content-type');
        let data;

        if (contentType && contentType.includes('application/json')) {
          // Handle JSON responses
          data = await response.json();
          // Extract text content from the JSON if it exists
          if (typeof data === 'object') {
            // Look for common fields that might contain the actual code
            if (data.code || data.content || data.text || data.data) {
              data = data.code || data.content || data.text || data.data;
            } else {
              // If no obvious text field, stringify the JSON
              data = JSON.stringify(data);
            }
          }
        } else {
          // Default to text for all other content types
          data = await response.text();
        }

        // Sanitize data - remove BOM and normalize line endings
        if (typeof data === 'string') {
          // Remove UTF-8 BOM if present
          if (data.charCodeAt(0) === 0xFEFF) {
            data = data.substring(1);
          }

          // Normalize line endings
          data = data.replace(/\r\n/g, '\n').replace(/\r/g, '\n');

          // Check if the content is base64 encoded
          if (/^[A-Za-z0-9+/=]+$/.test(data.trim())) {
            logToTerminal('Content is base64 encoded. Decoding...', true);
            data = atob(data); // Decode base64
          }
        }

        logToTerminal(`Firmware downloaded successfully (${typeof data === 'string' ? data.length : 'unknown'} bytes)`, true);
        return data;
      } catch (error) {
        console.error('Firmware fetch error:', error);
        logToTerminal(`Error fetching firmware: ${error.message}`, true);
        return null;
      }
    }

    // Function to handle file processing - works with both base64 and normal files - IMPROVED
    async function processFileContent(filename, fileContent) {
      try {
        // First check if content is actually a JSON string that needs parsing
        let processedContent = fileContent;

        try {
          // Try to parse as JSON in case it's a JSON response
          if (typeof fileContent === 'string' &&
            (fileContent.trim().startsWith('{') || fileContent.trim().startsWith('['))) {
            const jsonData = JSON.parse(fileContent);
            // If we parsed successfully, look for code/content fields
            if (typeof jsonData === 'object') {
              if (jsonData.code || jsonData.content || jsonData.script || jsonData.data) {
                processedContent = jsonData.code || jsonData.content || jsonData.script || jsonData.data;
                logToTerminal('Extracted code from JSON response', true);
              }
            }
          }
        } catch (jsonError) {
          // Not JSON or invalid JSON, continue with original content
        }

        // Now check if content is likely base64
        const isBase64 = typeof processedContent === 'string' &&
          /^[A-Za-z0-9+/=]+$/.test(processedContent.trim());

        let processedFilename = null;

        if (isBase64) {
          logToTerminal(`Content appears to be base64 encoded. Processing...`, true);

          // Transfer as base64 and decode on device
          processedFilename = await writeBase64ChunkedFile('RPC-install_{random-int}.py', processedContent);

          if (processedFilename) {
            // Wait a bit before executing
            logToTerminal(`Waiting for device to stabilize before execution...`, true);
            await new Promise(resolve => setTimeout(resolve, 2000));

            // Execute the file
            logToTerminal(`Starting execution of ${processedFilename}...`, true);
            await executeFile(processedFilename);

            // Additional delay after execution
            await new Promise(resolve => setTimeout(resolve, 2000));
          }
        } else {
          logToTerminal(`Content does not appear to be base64 encoded. Using regular transfer...`, true);

          // Generate a filename based on the original
          const sanitizedName = filename.replace(/[^a-zA-Z0-9._-]/g, '_');

          // Call existing code for regular file transfer
          processedFilename = await transferNormalFile(sanitizedName, processedContent);

          if (processedFilename && processedFilename.endsWith('.py')) {
            // Wait a bit before executing
            logToTerminal(`Waiting for device to stabilize before execution...`, true);
            await new Promise(resolve => setTimeout(resolve, 2000));

            // Execute the file
            logToTerminal(`Starting execution of ${processedFilename}...`, true);
            await executeFile(processedFilename);

            // Additional delay after execution
            await new Promise(resolve => setTimeout(resolve, 2000));
          }
        }

        // List files to confirm
        await sendCommand('\r\n', 300);
        await sendCommand("import os\r\n", 500);
        await sendCommand("print('Files on device:')\r\n", 500);
        await sendCommand("print(os.listdir())\r\n", 500);

        return processedFilename;
      } catch (error) {
        console.error('File processing error:', error);
        logToTerminal(`Error processing file: ${error.message}`, true);

        // Try to exit any modes we might be stuck in
        await sendCommand('\x04', 500); // Ctrl+D to exit paste mode if we're in it
        await sendCommand('\x03\r\n', 500); // Ctrl+C to interrupt

        return null;
      }
    }

    // Setup Install button to fetch and install firmware from the URL
    const installButton = document.getElementById('installButton');
    installButton.addEventListener('click', async () => {
      const firmwareUrl = document.getElementById('firmwareUrl').value;

      if (!firmwareUrl) {
        alert('Please enter a firmware URL in the settings menu.');
        return;
      }

      // Fetch the firmware
      const firmwareContent = await fetchFirmware(firmwareUrl);
      if (!firmwareContent) {
        alert('Failed to fetch firmware. Check the URL and try again.');
        return;
      }

      // Process and install the firmware
      const filename = await processFileContent('firmware.py', firmwareContent);

      if (filename) {
        logToTerminal(`Firmware installation complete!`, true);
      } else {
        logToTerminal(`Firmware installation failed.`, true);
      }
    });

    // Set up file upload functionality
    const fileInput = document.getElementById('fileInput');

    fileInput.addEventListener('change', async () => {
      const file = fileInput.files[0];
      if (!file) {
        logToTerminal('No file selected.', true);
        return;
      }

      // Reset the dropdown menu
      const dropdown = document.getElementById('firmwareFlavorDropdown');
      dropdown.value = '';
      updateChosenFlavor('Custom');

      logToTerminal(`Selected file: ${file.name}`, true);

      // Read file content
      const fileContent = await readFileAsText(file);

      // Process the file using our unified function
      const filename = await processFileContent(file.name, fileContent);

      if (filename) {
        logToTerminal(`Custom firmware installed as ${filename}`, true);
      } else {
        logToTerminal(`Custom firmware installation failed.`, true);
      }
    });

    // Original file transfer logic refactored into a function
    async function transferNormalFile(filename, fileContent) {
      if (fileContent.length > 200000) { // ~200KB limit
        logToTerminal(`Warning: File is large (${Math.round(fileContent.length / 1024)}KB). Transfer may take a while.`, true);
      }

      logToTerminal(`Starting regular file transfer...`, true);

      let transferMethod = '';
      let transferSuccessful = false;

      // Reset device and clear any running programs
      await sendCommand('\x03\x03\r\n', 500); // Double Ctrl+C
      await sendCommand('\r\n', 300); // Clear line

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
      } catch (err) {
        logToTerminal(`Paste mode not supported, falling back to line mode`, true);
        transferMethod = 'line';
      }

      // METHOD 1: PASTE MODE TRANSFER (most devices support this)
      if (transferMethod === 'paste') {
        try {
          logToTerminal(`Using paste mode transfer`, true);

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
        return filename;
      } else {
        logToTerminal(`File transfer failed. Please try again with a smaller file.`, true);
        return null;
      }
    }

    function readFileAsText(file) {
      return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(reader.result);
        reader.onerror = reject;
        reader.readAsText(file);
      });
    }

    // Function to fetch the firmware list from the JSON file
    async function fetchFirmwareList() {
      try {
        const response = await fetch('firmware_list.json');
        if (!response.ok) {
          throw new Error('Failed to fetch firmware list');
        }
        return await response.json();
      } catch (error) {
        console.error('Error fetching firmware list:', error);
        return [];
      }
    }

    // Function to populate the dropdown with firmware options
    function populateFirmwareDropdown(firmwareList) {
      const dropdown = document.getElementById('firmwareFlavorDropdown');
      dropdown.innerHTML = '<option value="">Select a Flavor</option>'; // Clear existing options

      firmwareList.forEach(firmware => {
        const option = document.createElement('option');
        option.value = firmware.url;
        option.textContent = firmware.flavor;
        dropdown.appendChild(option);
      });
    }

    // Function to update the "Chosen Flavor" text
    function updateChosenFlavor(flavor) {
      const flavorText = document.querySelector('.header.right .status-text:nth-child(3)');
      if (flavorText) {
        flavorText.textContent = `Chosen Flavor: ${flavor}`;
      }
    }

    // Function to handle dropdown selection
    function handleFirmwareSelection(event) {
      const selectedUrl = event.target.value;
      const firmwareList = event.target.firmwareList; // Attach firmwareList to the dropdown

      // Clear the custom firmware file input
      document.getElementById('fileInput').value = '';

      if (selectedUrl) {
        const selectedFirmware = firmwareList.find(firmware => firmware.url === selectedUrl);
        if (selectedFirmware) {
          updateChosenFlavor(selectedFirmware.flavor);
          document.getElementById('firmwareUrl').value = selectedFirmware.url;
        }
      } else {
        updateChosenFlavor('Custom'); // Default flavor if nothing is selected
      }
    }

    // Initialize the firmware dropdown
    async function initializeFirmwareDropdown() {
      const firmwareList = await fetchFirmwareList();
      const dropdown = document.getElementById('firmwareFlavorDropdown');

      if (firmwareList.length > 0) {
        populateFirmwareDropdown(firmwareList);
        dropdown.firmwareList = firmwareList; // Attach firmwareList to the dropdown
        dropdown.addEventListener('change', handleFirmwareSelection);
      } else {
        console.error('No firmware options available');
      }
    }

    // Call the initialization function when the document is loaded
    document.addEventListener('DOMContentLoaded', initializeFirmwareDropdown);

    // Handle URL input changes
    document.getElementById('firmwareUrl').addEventListener('input', (event) => {
      const selectedUrl = event.target.value;
      const dropdown = document.getElementById('firmwareFlavorDropdown');
      const firmwareList = dropdown.firmwareList;

      // Reset the dropdown menu
      dropdown.value = '';

      if (firmwareList) {
        const selectedFirmware = firmwareList.find(firmware => firmware.url === selectedUrl);
        if (selectedFirmware) {
          updateChosenFlavor(selectedFirmware.flavor);
        } else {
          updateChosenFlavor('Custom'); // Default flavor if the URL doesn't match any in the list
        }
      }
    });

    logToTerminal('Connected and ready for commands.', true);

  } catch (error) {
    console.error('Connection error:', error);
    alert(`Failed to connect to the device: ${error.message}`);
  }
});

// Make sure the uploadFirmwareBtn opens the file input
document.getElementById('uploadFirmwareBtn').addEventListener('click', () => {
  document.getElementById('fileInput').click();
});
