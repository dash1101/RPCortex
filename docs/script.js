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
    const portInfo = 'Serial Device';
    let terminalContainer = document.querySelector('.terminal-container');

    if (!terminalContainer) {
      terminalContainer = document.createElement('div');
      terminalContainer.className = 'terminal-container';
      document.body.appendChild(terminalContainer);

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
      if (message === null || message === undefined || message.trim() === '') return;
      const span = document.createElement('span');
      const className = isSystem ? 'system' : message.startsWith('>') ? 'command' : 'response';
      span.className = className;
      if (isSystem) {
        span.textContent = message;
        terminal.appendChild(span);
        terminal.appendChild(document.createElement('br'));
      } else {
        message = message.replace(/\r/g, '');
        const lines = message.split('\n');
        for (let i = 0; i < lines.length; i++) {
          if (i > 0) terminal.appendChild(document.createElement('br'));
          const lineSpan = document.createElement('span');
          lineSpan.className = className;
          lineSpan.textContent = lines[i];
          terminal.appendChild(lineSpan);
        }
      }
      terminal.scrollTop = terminal.scrollHeight;
    };

    const decoder = new TextDecoder();
    const encoder = new TextEncoder();

    const updateDeviceStatus = (portId, mpyVersion, memInfo) => {
      const statusElement = document.getElementById('device-status');
      if (!statusElement) return;
      if (!portId) {
        statusElement.innerHTML = `<div class="status-text">Device not yet initialized...</div>`;
        return;
      }
      let formattedVersion = '?.?.?';
      let mpyStatus = '';
      if (mpyVersion) {
        formattedVersion = mpyVersion;
        const versionParts = formattedVersion.split('.');
        if (versionParts.length >= 2) {
          const versionNumber = parseFloat(`${versionParts[0]}.${versionParts[1]}`);
          mpyStatus = !isNaN(versionNumber) && versionNumber >= 1.24 ? `<span class="status-good">(Good)</span>` : `<span class="status-warning">(Update recommended)</span>`;
        }
      }
      let ramValue = '?';
      let romValue = '?';
      let ramStatus = '';
      let romStatus = '';
      if (memInfo) {
        [ramValue, romValue] = memInfo;
        ramStatus = parseInt(ramValue) >= 128 ? `<span class="status-good">(Good)</span>` : `<span class="status-warning">(Limited)</span>`;
        romStatus = parseInt(romValue) >= 4 ? `<span class="status-good">(Good)</span>` : `<span class="status-warning">(Limited)</span>`;
      }
      statusElement.innerHTML = `
        <div class="status-text">Device connected at ${portId}</div>
        <div class="status-text">MPY: v${formattedVersion} ${mpyStatus}</div>
        <div class="status-text">RAM / ROM: ${ramValue}KB / ${romValue}MB ${ramStatus} / ${romStatus}</div>
      `;
    };

    const updateProgressInTitle = (percentage = null) => {
      if (percentage !== null) terminalTitle.textContent = `Terminal (${portInfo}) - ${percentage}%`;
      else terminalTitle.textContent = `Terminal (${portInfo})`;
    };

    const sendCommand = async (command, timeout = 500, waitForResponse = false) => {
      try {
        const lines = command.split('\n');
        if (lines.length > 1) {
          for (const line of lines) {
            if (line.trim()) {
              await writer.write(encoder.encode(line + '\r\n'));
              await new Promise(resolve => setTimeout(resolve, 100));
            }
          }
        } else await writer.write(encoder.encode(command));
        if (waitForResponse) return new Promise(resolve => pendingCommands.push({ command, resolver: resolve }));
        return await new Promise(resolve => setTimeout(resolve, timeout));
      } catch (error) {
        console.error('Command error:', error);
        logToTerminal(`Error sending command: ${error.message}`, true);
        return Promise.reject(error);
      }
    };

    const pendingCommands = [];
    let responseBuffer = '';

    const readLoop = async () => {
      try {
        updateDeviceStatus(portInfo, null);
        logToTerminal(`Connected to device. Initializing...`, true);
        await sendCommand('\x03\x03\r\n', 1000);
        await sendCommand('\r\n', 300);
        await sendCommand('import sys\r\n', 500);
        await sendCommand('print("MPYVER:", ".".join([str(i) for i in sys.implementation.version[:3]]))\r\n', 500);
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
          const text = decoder.decode(value);
          rawBuffer += text;
          responseBuffer += text;
          if (!isProcessingInfo || !(rawBuffer.includes("MPYVER:") || rawBuffer.includes("import") || rawBuffer.includes("try:"))) logToTerminal(text);
          const mpyMatch = rawBuffer.match(/MPYVER:\s*([0-9.]+)/);
          if (mpyMatch) {
            mpyVersion = mpyMatch[1];
            logToTerminal(`Found MPY version: v${mpyVersion}`, true);
            rawBuffer = rawBuffer.replace(/MPYVER:\s*([0-9.]+)/, '');
            updateDeviceStatus(portInfo, mpyVersion);
            isProcessingInfo = false;
            logToTerminal("Device initialization complete", true);
            await sendCommand('\r\n', 300);
          }
          if (pendingCommands.length > 0 && responseBuffer.includes('>>>')) {
            const command = pendingCommands.shift();
            if (command && command.resolver) {
              command.resolver(responseBuffer);
              responseBuffer = '';
            }
          }
          if (rawBuffer.length > 5000) rawBuffer = rawBuffer.slice(-2000);
        }
      } catch (error) {
        console.error('Read error:', error);
        logToTerminal(`Error reading from device: ${error.message}`, true);
        updateDeviceStatus(null);
      } finally {
        reader.releaseLock();
      }
    };

    readLoop();

    sendButton.addEventListener('click', handleSendCommand);
    inputField.addEventListener('keypress', (event) => {
      if (event.key === 'Enter') handleSendCommand();
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

    const checkDeviceCapabilities = async () => {
      try {
        await sendCommand('import binascii\r\n', 300);
        await sendCommand('import gc\r\n', 300);
        return { hasBase64: true };
      } catch (error) {
        return { hasBase64: false };
      }
    };

    async function writeBase64ChunkedFile(filename, base64Data) {
      logToTerminal(`Starting base64 file transfer for ${filename}...`, true);
      try {
        const randomNum = Math.floor(Math.random() * 10000);
        const actualFilename = filename.replace('{random-int}', randomNum);
        await sendCommand('\x03\x03\r\n', 1000);
        await sendCommand('\r\n', 500);
        const capabilities = await checkDeviceCapabilities();
        const CHUNK_SIZE = 256;
        let position = 0;
        let fileOpened = false;
        await sendCommand(`f = open('${actualFilename}', 'wb')\r\n`, 1000);
        fileOpened = true;
        while (position < base64Data.length) {
          const end = Math.min(position + CHUNK_SIZE, base64Data.length);
          const chunk = base64Data.substring(position, end);
          if (capabilities.hasBase64) {
            await sendCommand(`f.write(binascii.a2b_base64('${chunk}'))\r\n`, 1000);
            await sendCommand('gc.collect()\r\n', 300);
          } else await sendCommand(`f.write(bytes([int(x) for x in '${chunk}'.encode('utf-8')]))\r\n`, 1500);
          position = end;
          const percent = Math.min(100, Math.round((position / base64Data.length) * 100));
          if (position % (CHUNK_SIZE * 5) === 0 || position >= base64Data.length) {
            logToTerminal(`Transfer progress: ${percent}%`, true);
            updateProgressInTitle(percent);
          }
          await new Promise(resolve => setTimeout(resolve, 100));
        }
        await new Promise(resolve => setTimeout(resolve, 500));
        await sendCommand('f.close()\r\n', 1000);
        await new Promise(resolve => setTimeout(resolve, 1000));
        logToTerminal(`Base64 file transferred and saved as ${actualFilename}`, true);
        updateProgressInTitle();
        return actualFilename;
      } catch (error) {
        console.error('Base64 file transfer error:', error);
        logToTerminal(`Error in base64 file transfer: ${error.message}`, true);
        updateProgressInTitle();
        await sendCommand('\x03\r\n', 500);
        await sendCommand('try:\n    f.close()\nexcept:\n    pass\r\n', 500);
        return null;
      }
    }

    async function executeFile(filename) {
      try {
        logToTerminal(`Executing ${filename}...`, true);
        await sendCommand('\x03\x03\r\n', 1000);
        await new Promise(resolve => setTimeout(resolve, 500));
        await sendCommand('\r\n', 500);
        await new Promise(resolve => setTimeout(resolve, 500));
        const execCommand = `try: exec(open('${filename}').read())\nexcept Exception as e: print('Execution error:', repr(e))\r\n`;
        await sendCommand(execCommand, 5000);
        await new Promise(resolve => setTimeout(resolve, 3000));
        logToTerminal(`Execution of ${filename} complete`, true);
        return true;
      } catch (error) {
        console.error('Execution error:', error);
        logToTerminal(`Error executing ${filename}: ${error.message}`, true);
        try {
          logToTerminal(`Trying alternate execution method...`, true);
          await sendCommand('\x03\r\n', 500);
          await sendCommand('\r\n', 500);
          await sendCommand(`exec(open('${filename}').read())\r\n`, 5000);
          await new Promise(resolve => setTimeout(resolve, 3000));
          logToTerminal(`Alternate execution complete`, true);
          return true;
        } catch (altError) {
          console.error('Alternate execution error:', altError);
          logToTerminal(`Error with alternate execution: ${altError.message}`, true);
          await sendCommand('\x03\r\n', 500);
          await sendCommand('\r\n', 500);
          return false;
        }
      }
    }

    async function fetchFirmware(url) {
      logToTerminal(`Fetching firmware from ${url}...`, true);
      try {
        const response = await fetch(url);
        if (!response.ok) throw new Error(`Network response was not ok: ${response.status}`);
        const contentType = response.headers.get('content-type');
        let data;
        if (contentType && contentType.includes('application/json')) {
          data = await response.json();
          if (typeof data === 'object') data = data.code || data.content || data.text || data.data || JSON.stringify(data);
        } else data = await response.text();
        if (typeof data === 'string') {
          if (data.charCodeAt(0) === 0xFEFF) data = data.substring(1);
          data = data.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
          if (/^[A-Za-z0-9+/=]+$/.test(data.trim())) {
            logToTerminal('Content is base64 encoded. Decoding...', true);
            data = atob(data);
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

    async function processFileContent(filename, fileContent) {
      try {
        let processedContent = fileContent;
        try {
          if (typeof fileContent === 'string' && (fileContent.trim().startsWith('{') || fileContent.trim().startsWith('['))) {
            const jsonData = JSON.parse(fileContent);
            if (typeof jsonData === 'object') processedContent = jsonData.code || jsonData.content || jsonData.script || jsonData.data;
          }
        } catch (jsonError) {}
        const isBase64 = typeof processedContent === 'string' && /^[A-Za-z0-9+/=]+$/.test(processedContent.trim());
        let processedFilename = null;
        if (isBase64) {
          processedFilename = await writeBase64ChunkedFile('RPC-install_{random-int}.py', processedContent);
          if (processedFilename) {
            await new Promise(resolve => setTimeout(resolve, 2000));
            await executeFile(processedFilename);
            await new Promise(resolve => setTimeout(resolve, 2000));
          }
        } else {
          const sanitizedName = filename.replace(/[^a-zA-Z0-9._-]/g, '_');
          processedFilename = await transferNormalFile(sanitizedName, processedContent);
          if (processedFilename && processedFilename.endsWith('.py')) {
            await new Promise(resolve => setTimeout(resolve, 2000));
            await executeFile(processedFilename);
            await new Promise(resolve => setTimeout(resolve, 2000));
          }
        }
        await sendCommand('\r\n', 300);
        await sendCommand("import os\r\n", 500);
        await sendCommand("print('Files on device:')\r\n", 500);
        await sendCommand("print(os.listdir())\r\n", 500);
        return processedFilename;
      } catch (error) {
        console.error('File processing error:', error);
        logToTerminal(`Error processing file: ${error.message}`, true);
        await sendCommand('\x04', 500);
        await sendCommand('\x03\r\n', 500);
        return null;
      }
    }

    const installButton = document.getElementById('installButton');
    installButton.addEventListener('click', async () => {
      const firmwareUrl = document.getElementById('firmwareUrl').value;
      if (!firmwareUrl) {
        alert('Please enter a firmware URL in the settings menu.');
        return;
      }
      const firmwareContent = await fetchFirmware(firmwareUrl);
      if (!firmwareContent) {
        alert('Failed to fetch firmware. Check the URL and try again.');
        return;
      }
      const filename = await processFileContent('firmware.py', firmwareContent);
      if (filename) logToTerminal(`Firmware installation complete!`, true);
      else logToTerminal(`Firmware installation failed.`, true);
    });

    const fileInput = document.getElementById('fileInput');
    fileInput.addEventListener('change', async () => {
      const file = fileInput.files[0];
      if (!file) {
        logToTerminal('No file selected.', true);
        return;
      }
      logToTerminal(`Selected file: ${file.name}`, true);
      const fileContent = await readFileAsText(file);
      const filename = await processFileContent(file.name, fileContent);
      if (filename) logToTerminal(`Custom firmware installed as ${filename}`, true);
      else logToTerminal(`Custom firmware installation failed.`, true);
    });

    async function transferNormalFile(filename, fileContent) {
      if (fileContent.length > 200000) logToTerminal(`Warning: File is large (${Math.round(fileContent.length / 1024)}KB). Transfer may take a while.`, true);
      logToTerminal(`Starting regular file transfer...`, true);
      let transferMethod = '';
      let transferSuccessful = false;
      await sendCommand('\x03\x03\r\n', 500);
      await sendCommand('\r\n', 300);
      try {
        await sendCommand('\x05', 1000);
        transferMethod = 'paste';
        await sendCommand('\x04', 500);
        await sendCommand('\r\n', 300);
      } catch (err) {
        transferMethod = 'line';
      }
      if (transferMethod === 'paste') {
        try {
          await sendCommand('\x05', 1000);
          await sendCommand(`with open('${filename}', 'w') as f:\n`, 500);
          const CHUNK_SIZE = 512;
          let position = 0;
          while (position < fileContent.length) {
            let chunk = fileContent.slice(position, position + CHUNK_SIZE).replace(/\\/g, '\\\\').replace(/'/g, "\\'").replace(/\r?\n/g, '\\n');
            await sendCommand(`    f.write('${chunk}')\n`, 10);
            position += CHUNK_SIZE;
            if (position % (CHUNK_SIZE * 10) === 0 || position >= fileContent.length) {
              const percent = Math.min(100, Math.round((position / fileContent.length) * 100));
              logToTerminal(`Transfer progress: ${percent}%`, true);
              updateProgressInTitle(percent);
            }
          }
          await sendCommand('\x04', 1000);
          await sendCommand('\r\n', 500);
          transferSuccessful = true;
          logToTerminal(`File transfer complete using paste mode`, true);
          updateProgressInTitle();
        } catch (error) {
          transferMethod = 'line';
        }
      }
      if (transferMethod === 'line' && !transferSuccessful) {
        try {
          await sendCommand('\x03\x03\r\n', 500);
          await sendCommand('\r\n', 300);
          await sendCommand(`f = open('${filename}', 'w')\r\n`, 500);
          const lines = fileContent.split(/\r?\n/);
          for (let i = 0; i < lines.length; i++) {
            const line = lines[i].replace(/\\/g, '\\\\').replace(/'/g, "\\'");
            await sendCommand(`f.write('${line}${i < lines.length - 1 ? "\\n" : ""}')\r\n`, 100);
            if (i % 10 === 0 || i >= lines.length - 1) {
              const percent = Math.min(100, Math.round(((i + 1) / lines.length) * 100));
              logToTerminal(`Transfer progress: ${percent}%`, true);
              updateProgressInTitle(percent);
            }
          }
          await sendCommand(`f.close()\r\n`, 500);
          transferSuccessful = true;
          logToTerminal(`File transfer complete using line mode`, true);
          updateProgressInTitle();
        } catch (error) {
          console.error('Line mode transfer error:', error);
          logToTerminal(`Error in line mode transfer: ${error.message}`, true);
          updateProgressInTitle();
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

    logToTerminal('Connected and ready for commands.', true);
  } catch (error) {
    console.error('Connection error:', error);
    alert(`Failed to connect to the device: ${error.message}`);
  }
});

document.addEventListener('DOMContentLoaded', async () => {
  let flavors = [];
  let selectedFlavor = null;

  const fetchFlavors = async () => {
    try {
      const response = await fetch('https://raw.githubusercontent.com/dash1101/RPCortex/refs/heads/main/docs/flavors.json');
      flavors = await response.json();
      populateFlavorDropdown();
    } catch (error) {
      console.error('Failed to fetch flavors:', error);
    }
  };

  const populateFlavorDropdown = () => {
    const firmwareFlavorSelect = document.getElementById('firmwareFlavor');
    firmwareFlavorSelect.innerHTML = '<option value="custom">Custom</option>';
    flavors.forEach(flavor => {
      const option = document.createElement('option');
      option.value = flavor.url;
      option.textContent = `${flavor.flavor} (v${flavor.version})`;
      firmwareFlavorSelect.appendChild(option);
    });
  };

  const handleFlavorSelection = (event) => {
    const firmwareUrlInput = document.getElementById('firmwareUrl');
    const uploadFirmwareBtn = document.getElementById('uploadFirmwareBtn');
    if (event.target.value === 'custom') {
      firmwareUrlInput.disabled = false;
      uploadFirmwareBtn.disabled = false;
      firmwareUrlInput.value = '';
      selectedFlavor = null;
      updateMainPageFlavor('Custom');
    } else {
      selectedFlavor = flavors.find(flavor => flavor.url === event.target.value);
      firmwareUrlInput.disabled = true;
      uploadFirmwareBtn.disabled = true;
      firmwareUrlInput.value = selectedFlavor.url;
      updateMainPageFlavor(selectedFlavor.flavor);
    }
  };

  const updateMainPageFlavor = (flavorName) => {
    const flavorText = document.querySelector('.header.right .status-text:nth-child(3)');
    if (flavorText) flavorText.textContent = `Chosen Flavor: ${flavorName}`;
  };

  const maximizeTerminal = () => {
    const terminalContainer = document.querySelector('.terminal-container');
    terminalContainer.classList.add('fullscreen');
    document.body.classList.add('blur');
  };

  const restoreTerminal = () => {
    const terminalContainer = document.querySelector('.terminal-container');
    terminalContainer.classList.remove('fullscreen');
    document.body.classList.remove('blur');
  };

  const fetchMemoryInfo = async () => {
    try {
      await sendCommand('import gc\r\n');
      const ramResponse = await sendCommand('print(gc.mem_alloc() + gc.mem_free())\r\n', 1000, true);
      const totalRamKB = parseInt(ramResponse.trim(), 10) / 1024;
      const freeRamResponse = await sendCommand('print(gc.mem_free())\r\n', 1000, true);
      const freeRamKB = parseInt(freeRamResponse.trim(), 10) / 1024;
      await sendCommand('import os\r\n');
      const storageResponse = await sendCommand('print(os.statvfs("/").f_bsize * os.statvfs("/").f_bavail)\r\n', 1000, true);
      const storageValue = parseInt(storageResponse.trim(), 10) / (1024 * 1024);
      updateMemoryInfo(totalRamKB.toFixed(2), freeRamKB.toFixed(2), storageValue.toFixed(2));
    } catch (error) {
      console.error('Error fetching memory info:', error);
      updateMemoryInfo('?', '?', '?');
    }
  };

  const updateMemoryInfo = (totalRamKB, freeRamKB, storageValue) => {
    const memoryInfoElement = document.querySelector('.header.left .status-text');
    if (memoryInfoElement) memoryInfoElement.textContent = `RAM: ${freeRamKB}KB / ${totalRamKB}KB | Storage: ${storageValue}MB`;
  };

  const installFirmware = async (url) => {
    maximizeTerminal();
    try {
      const firmwareContent = await fetchFirmware(url);
      if (!firmwareContent) throw new Error('Failed to fetch firmware.');
      const filename = await processFileContent('firmware.py', firmwareContent);
      if (filename) logToTerminal(`Firmware installation complete!`, true);
      else logToTerminal(`Firmware installation failed.`, true);
    } catch (error) {
      console.error('Installation error:', error);
      logToTerminal(`Error during installation: ${error.message}`, true);
    } finally {
      restoreTerminal();
    }
  };

  const handleInstallButtonClick = async () => {
    const firmwareFlavorSelect = document.getElementById('firmwareFlavor');
    const firmwareUrlInput = document.getElementById('firmwareUrl');
    const fileInput = document.getElementById('fileInput');
    if (firmwareFlavorSelect.value === 'custom') {
      if (fileInput.files.length > 0) {
        const file = fileInput.files[0];
        const fileContent = await readFileAsText(file);
        await processFileContent(file.name, fileContent);
      } else if (firmwareUrlInput.value) await installFirmware(firmwareUrlInput.value);
      else alert('Please provide a custom URL or upload a file.');
    } else await installFirmware(selectedFlavor.url);
  };

  const initializeEventListeners = () => {
    const firmwareFlavorSelect = document.getElementById('firmwareFlavor');
    const installButton = document.getElementById('installButton');
    const uploadFirmwareBtn = document.getElementById('uploadFirmwareBtn');
    firmwareFlavorSelect.addEventListener('change', handleFlavorSelection);
    installButton.addEventListener('click', handleInstallButtonClick);
    uploadFirmwareBtn.addEventListener('click', () => document.getElementById('fileInput').click());
  };

  await fetchFlavors();
  initializeEventListeners();

  document.getElementById('connectButton').addEventListener('click', async () => {
    try {
      await connectToDevice();
      await fetchMemoryInfo();
    } catch (error) {
      console.error('Connection error:', error);
    }
  });
});

function readFileAsText(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result);
    reader.onerror = reject;
    reader.readAsText(file);
  });
}
