#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-C3 Serial Console</title>
<style>
  :root {
    --bg-primary: #0f111a;
    --bg-secondary: #1a1c29;
    --bg-tertiary: #23263b;
    --border-color: #2e324a;
    --text-primary: #e6e8f0;
    --text-muted: #8b92ad;
    --accent-color: #6366f1;
    --accent-hover: #4f46e5;
    --accent-light: rgba(99, 102, 241, 0.15);
    --status-green: #10b981;
    --status-red: #ef4444;
    --status-yellow: #f59e0b;
    --cmd-color: #38bdf8;
    --sys-color: #a78bfa;
    --ha-color: #c084fc;
    --slave-color: #34d399;
    --debug-color: #94a3b8;
    --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background-color: var(--bg-primary);
    color: var(--text-primary);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    height: 100vh;
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  /* Header */
  header {
    background-color: var(--bg-secondary);
    border-bottom: 1px solid var(--border-color);
    padding: 10px 16px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 10px;
    z-index: 10;
  }

  .title-group {
    display: flex;
    align-items: center;
    gap: 10px;
  }

  .title-icon {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 28px;
    height: 28px;
    border-radius: 6px;
    background: linear-gradient(135deg, #6366f1, #8b5cf6);
    color: #fff;
    font-size: 0.95rem;
    font-weight: bold;
  }

  h1 {
    font-size: 1.02rem;
    font-weight: 600;
    letter-spacing: -0.01em;
    color: #fff;
  }

  .status-badge {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 0.76rem;
    font-weight: 500;
    padding: 3px 9px;
    border-radius: 9999px;
    background-color: rgba(239, 68, 68, 0.15);
    color: var(--status-red);
    transition: all 0.2s ease;
  }

  .status-badge.connected {
    background-color: rgba(16, 185, 129, 0.15);
    color: var(--status-green);
  }

  .status-badge.connecting {
    background-color: rgba(245, 158, 11, 0.15);
    color: var(--status-yellow);
  }

  .status-dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background-color: currentColor;
  }

  .status-badge.connected .status-dot {
    box-shadow: 0 0 6px currentColor;
    animation: pulse 2s infinite;
  }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.4; }
  }

  .controls-group {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
  }

  .badge-info {
    font-family: var(--font-mono);
    font-size: 0.74rem;
    background-color: var(--bg-tertiary);
    color: var(--text-muted);
    padding: 4px 8px;
    border-radius: 4px;
    border: 1px solid var(--border-color);
  }

  .btn {
    background-color: var(--bg-tertiary);
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    padding: 5px 10px;
    border-radius: 6px;
    font-size: 0.78rem;
    font-weight: 500;
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    gap: 5px;
    transition: background-color 0.15s, border-color 0.15s;
    user-select: none;
  }

  .btn:hover {
    background-color: var(--border-color);
    border-color: #3f4463;
  }

  .btn:active {
    transform: translateY(1px);
  }

  .btn.active {
    background-color: rgba(245, 158, 11, 0.2);
    border-color: var(--status-yellow);
    color: var(--status-yellow);
  }

  .toggle-label {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: 0.78rem;
    color: var(--text-muted);
    cursor: pointer;
    user-select: none;
  }

  .toggle-label input[type="checkbox"] {
    accent-color: var(--accent-color);
    cursor: pointer;
  }

  /* Main Terminal Area */
  main {
    flex: 1;
    position: relative;
    overflow: hidden;
    display: flex;
    flex-direction: column;
  }

  #terminal {
    flex: 1;
    overflow-y: auto;
    overflow-x: auto;
    padding: 12px 16px;
    font-family: var(--font-mono);
    font-size: 0.85rem;
    line-height: 1.5;
    white-space: pre-wrap;
    word-break: break-all;
    background-color: var(--bg-primary);
    color: #cbd5e1;
  }

  #terminal::-webkit-scrollbar {
    width: 8px;
    height: 8px;
  }

  #terminal::-webkit-scrollbar-track {
    background: var(--bg-primary);
  }

  #terminal::-webkit-scrollbar-thumb {
    background: var(--bg-tertiary);
    border-radius: 4px;
  }

  #terminal::-webkit-scrollbar-thumb:hover {
    background: var(--border-color);
  }

  /* Source categories */
  .line-ha {
    color: var(--ha-color);
  }

  .line-user {
    color: var(--cmd-color);
    font-weight: 600;
  }

  .line-slave {
    color: var(--slave-color);
  }

  .line-debug {
    color: var(--debug-color);
  }

  .line-sys {
    color: var(--sys-color);
    font-style: italic;
  }

  .src-tag {
    display: inline-block;
    min-width: 2.7em;
    text-align: center;
    font-family: var(--font-mono);
    font-size: 0.64rem;
    font-weight: 700;
    letter-spacing: 0.04em;
    padding: 0 4px;
    margin-right: 6px;
    border-radius: 3px;
    border: 1px solid currentColor;
    opacity: 0.85;
    user-select: none;
  }

  .tag-ha { color: var(--ha-color); }
  .tag-user { color: var(--cmd-color); }
  .tag-slave { color: var(--slave-color); }
  .tag-debug { color: var(--debug-color); }

  /* Source visibility filters */
  #terminal.hide-ha .src-ha,
  #terminal.hide-user .src-user,
  #terminal.hide-slave .src-slave,
  #terminal.hide-debug .src-debug {
    display: none;
  }

  .filter-chip {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    font-family: var(--font-mono);
    font-size: 0.7rem;
    font-weight: 600;
    letter-spacing: 0.04em;
    padding: 2px 9px;
    border-radius: 9999px;
    border: 1px solid currentColor;
    background: transparent;
    cursor: pointer;
    user-select: none;
    opacity: 0.4;
    transition: opacity 0.15s ease, background-color 0.15s ease;
  }

  .filter-chip input {
    display: none;
  }

  .filter-chip.active {
    opacity: 1;
    background-color: rgba(255, 255, 255, 0.06);
  }

  .filter-ha { color: var(--ha-color); }
  .filter-user { color: var(--cmd-color); }
  .filter-slave { color: var(--slave-color); }
  .filter-debug { color: var(--debug-color); }

  .timestamp {
    color: var(--text-muted);
    margin-right: 6px;
    user-select: none;
    font-size: 0.76rem;
  }

  /* Macro quick commands */
  .macro-bar {
    background-color: var(--bg-secondary);
    border-top: 1px solid var(--border-color);
    padding: 6px 14px;
    display: flex;
    align-items: center;
    gap: 6px;
    overflow-x: auto;
    white-space: nowrap;
  }

  .macro-bar::-webkit-scrollbar {
    display: none;
  }

  .macro-label {
    font-size: 0.7rem;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--text-muted);
    margin-right: 4px;
  }

  .pill-btn {
    background-color: var(--bg-tertiary);
    border: 1px solid var(--border-color);
    color: var(--text-muted);
    padding: 2px 10px;
    border-radius: 9999px;
    font-size: 0.74rem;
    font-family: var(--font-mono);
    cursor: pointer;
    transition: all 0.15s ease;
  }

  .pill-btn:hover {
    background-color: var(--accent-light);
    border-color: var(--accent-color);
    color: #fff;
  }

  /* Footer & Command Input */
  footer {
    background-color: var(--bg-secondary);
    border-top: 1px solid var(--border-color);
    padding: 10px 16px;
    display: flex;
    gap: 10px;
    align-items: center;
  }

  .prompt-symbol {
    font-family: var(--font-mono);
    font-weight: 700;
    color: var(--accent-color);
    font-size: 1.1rem;
    user-select: none;
  }

  .input-wrapper {
    flex: 1;
    display: flex;
  }

  #cmdInput {
    width: 100%;
    background-color: var(--bg-primary);
    border: 1px solid var(--border-color);
    border-radius: 6px;
    color: #fff;
    padding: 8px 12px;
    font-family: var(--font-mono);
    font-size: 0.88rem;
    outline: none;
    transition: border-color 0.2s, box-shadow 0.2s;
  }

  #cmdInput:focus {
    border-color: var(--accent-color);
    box-shadow: 0 0 0 2px var(--accent-light);
  }

  select.line-ending-select {
    background-color: var(--bg-tertiary);
    border: 1px solid var(--border-color);
    color: var(--text-muted);
    border-radius: 6px;
    padding: 7px 10px;
    font-size: 0.8rem;
    outline: none;
    cursor: pointer;
  }

  .btn-send {
    background-color: var(--accent-color);
    color: #fff;
    border: 1px solid var(--accent-color);
    padding: 7px 16px;
    font-weight: 600;
  }

  .btn-send:hover {
    background-color: var(--accent-hover);
    border-color: var(--accent-hover);
  }
</style>
</head>
<body>

<header>
  <div class="title-group">
    <div class="title-icon">>_</div>
    <h1>ESP32-C3 Serial Console</h1>
    <div id="statusBadge" class="status-badge connecting">
      <span class="status-dot"></span>
      <span id="statusText">Connecting...</span>
    </div>
  </div>

  <div class="controls-group">
    <span class="badge-info">115200 baud</span>
    <label class="toggle-label">
      <input type="checkbox" id="autoscrollCb" checked> Auto-scroll
    </label>
    <label class="toggle-label">
      <input type="checkbox" id="timestampsCb"> Timestamps
    </label>
    <button id="pauseBtn" class="btn" title="Pause terminal updates">⏸ Pause</button>
    <button id="clearBtn" class="btn" title="Clear terminal output">🗑 Clear</button>
    <button id="exportBtn" class="btn" title="Download log as .txt">💾 Export</button>
  </div>
</header>

<main>
  <div id="terminal"></div>
</main>

<div class="macro-bar">
  <span class="macro-label">Sources:</span>
  <label class="filter-chip filter-ha active" title="Home Assistant requests">
    <input type="checkbox" id="fHa" checked>HA
  </label>
  <label class="filter-chip filter-user active" title="Commands typed in the console">
    <input type="checkbox" id="fUser" checked>CMD
  </label>
  <label class="filter-chip filter-slave active" title="RS485 slave responses">
    <input type="checkbox" id="fSlave" checked>SLV
  </label>
  <label class="filter-chip filter-debug active" title="Firmware debug prints">
    <input type="checkbox" id="fDebug" checked>DBG
  </label>
  <span class="macro-label" style="margin-left:12px">Quick cmds:</span>
  <button class="pill-btn" onclick="sendCommand('help')">help</button>
  <button class="pill-btn" onclick="sendCommand('ip')">ip</button>
  <button class="pill-btn" onclick="sendCommand('heap')">heap</button>
  <button class="pill-btn" onclick="sendCommand('uptime')">uptime</button>
  <button class="pill-btn" onclick="sendCommand('restart')">restart</button>
  <button class="pill-btn" onclick="clearTerminal()">clear</button>
</div>

<footer>
  <span class="prompt-symbol">&gt;</span>
  <div class="input-wrapper">
    <input type="text" id="cmdInput" placeholder="Type command... (Enter to send, ↑↓ for history)" autocomplete="off" spellcheck="false">
  </div>
  <select id="lineEndingSelect" class="line-ending-select" title="Line Ending">
    <option value="lf">Newline (LF \n)</option>
    <option value="crlf">Both (CRLF \r\n)</option>
    <option value="cr">Return (CR \r)</option>
    <option value="none">No Ending</option>
  </select>
  <button id="sendBtn" class="btn btn-send">Send</button>
</footer>

<script>
(() => {
  let ws = null;
  let reconnectTimer = null;
  let isPaused = false;
  let atLineStart = true;
  const history = [];
  let historyIdx = -1;
  const MAX_LINES = 2500;

  // Tagged-line protocol: 0x01 <tag> <text> '\n'
  const SOURCE_TAGS = { H: 'ha', U: 'user', S: 'slave', D: 'debug' };
  const SOURCE_LABELS = { ha: 'HA', user: 'CMD', slave: 'SLV', debug: 'DBG' };
  let rxBuffer = '';

  const terminal = document.getElementById('terminal');
  const statusBadge = document.getElementById('statusBadge');
  const statusText = document.getElementById('statusText');
  const cmdInput = document.getElementById('cmdInput');
  const autoscrollCb = document.getElementById('autoscrollCb');
  const timestampsCb = document.getElementById('timestampsCb');
  const lineEndingSelect = document.getElementById('lineEndingSelect');
  const pauseBtn = document.getElementById('pauseBtn');
  const clearBtn = document.getElementById('clearBtn');
  const exportBtn = document.getElementById('exportBtn');
  const sendBtn = document.getElementById('sendBtn');
  const filterCbs = {
    ha: document.getElementById('fHa'),
    user: document.getElementById('fUser'),
    slave: document.getElementById('fSlave'),
    debug: document.getElementById('fDebug')
  };

  function getTimestamp() {
    const d = new Date();
    const pad = (n, len = 2) => String(n).padStart(len, '0');
    return `[${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}] `;
  }

  function updateStatus(state, msg) {
    statusBadge.className = 'status-badge ' + state;
    statusText.textContent = msg;
  }

  function ansiToHtml(text) {
    const escaped = text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    const colors = {
      '30': '#4b5563', '31': '#ef4444', '32': '#22c55e', '33': '#eab308',
      '34': '#3b82f6', '35': '#a855f7', '36': '#06b6d4', '37': '#f3f4f6',
      '90': '#6b7280', '91': '#f87171', '92': '#4ade80', '93': '#facc15',
      '94': '#60a5fa', '95': '#c084fc', '96': '#22d3ee', '97': '#ffffff'
    };
    return escaped.replace(/\x1b\[([0-9;]+)m/g, (_, codeStr) => {
      const parts = codeStr.split(';');
      for (const p of parts) {
        if (p === '0') return '</span>';
        if (colors[p]) return `<span style="color:${colors[p]}">`;
        if (p === '1') return '<span style="font-weight:bold">';
      }
      return '';
    });
  }

  function appendSpan(htmlContent, className = '') {
    const span = document.createElement('span');
    if (className) span.className = className;
    span.innerHTML = htmlContent;
    terminal.appendChild(span);
    trimLines();
    scrollTerminal();
  }

  function appendSourceLine(src, text) {
    if (!atLineStart) appendSpan('\n');

    // User commands are shown with a prompt, whether they were typed in the
    // web console or on the physical USB serial console.
    const body = (src === 'user') ? ('> ' + text) : text;

    const ts = timestampsCb.checked
      ? `<span class="timestamp">${getTimestamp()}</span>`
      : '';
    const tag = `<span class="src-tag tag-${src}">${SOURCE_LABELS[src]}</span>`;
    appendSpan(ts + tag + ' ' + ansiToHtml(body) + '\n', `line-${src} src-${src}`);
    atLineStart = true;
  }

  function handleIncoming(data) {
    rxBuffer += data;

    let nl;
    while ((nl = rxBuffer.indexOf('\n')) !== -1) {
      const raw = rxBuffer.slice(0, nl);
      rxBuffer = rxBuffer.slice(nl + 1);

      let src = 'debug';
      let text = raw;
      if (raw.charCodeAt(0) === 1 && raw.length >= 2) {
        src = SOURCE_TAGS[raw[1]] || 'debug';
        text = raw.slice(2);
      }

      appendSourceLine(src, text);
    }

    // Do not let a missing terminator grow without bound.
    if (rxBuffer.length > 8192) rxBuffer = '';
  }

  function applySourceFilters() {
    for (const [src, cb] of Object.entries(filterCbs)) {
      if (!cb) continue;
      terminal.classList.toggle('hide-' + src, !cb.checked);
      const chip = cb.closest('.filter-chip');
      if (chip) chip.classList.toggle('active', cb.checked);
    }
  }

  function appendSystemLine(text) {
    if (!atLineStart) appendSpan('\n');
    if (timestampsCb.checked) appendSpan(getTimestamp(), 'timestamp');
    appendSpan(ansiToHtml(text) + '\n', 'line-sys');
    atLineStart = true;
  }

  function trimLines() {
    if (terminal.childNodes.length > MAX_LINES) {
      const removeCount = terminal.childNodes.length - MAX_LINES + 200;
      for (let i = 0; i < removeCount; i++) {
        if (terminal.firstChild) terminal.removeChild(terminal.firstChild);
      }
    }
  }

  function scrollTerminal() {
    if (autoscrollCb.checked && !isPaused) {
      terminal.scrollTop = terminal.scrollHeight;
    }
  }

  function connect() {
    if (reconnectTimer) clearTimeout(reconnectTimer);
    updateStatus('connecting', 'Connecting...');

    const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = location.hostname || '127.0.0.1';
    const wsUrl = `${protocol}//${host}:81/`;

    try {
      ws = new WebSocket(wsUrl);
    } catch (e) {
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      rxBuffer = '';
      updateStatus('connected', 'Connected');
      appendSystemLine(`[WebSocket connected to ${wsUrl}]`);
    };

    ws.onmessage = (event) => {
      if (isPaused) return;
      handleIncoming(event.data);
    };

    ws.onclose = () => {
      updateStatus('disconnected', 'Disconnected');
      appendSystemLine('[WebSocket disconnected]');
      scheduleReconnect();
    };

    ws.onerror = () => {
      ws.close();
    };
  }

  function scheduleReconnect() {
    if (reconnectTimer) clearTimeout(reconnectTimer);
    reconnectTimer = setTimeout(connect, 2000);
  }

  window.sendCommand = function(cmd = null) {
    const isExplicit = (cmd !== null);
    const text = isExplicit ? cmd : cmdInput.value;
    if (!text && !isExplicit) return;

    if (!isExplicit) {
      history.push(text);
      if (history.length > 50) history.shift();
      historyIdx = history.length;
      cmdInput.value = '';
    }

    // The command is echoed back by the firmware as a USER line, so it is
    // displayed (and filtered/exported) like every other source.

    let payload = text;
    const le = lineEndingSelect.value;
    if (le === 'lf') payload += '\n';
    else if (le === 'crlf') payload += '\r\n';
    else if (le === 'cr') payload += '\r';

    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(payload);
    } else {
      appendSystemLine('[Error: Cannot send - WebSocket is not connected]');
    }
  };

  window.clearTerminal = function() {
    terminal.innerHTML = '';
    atLineStart = true;
  };

  clearBtn.onclick = window.clearTerminal;

  pauseBtn.onclick = () => {
    isPaused = !isPaused;
    if (isPaused) {
      pauseBtn.textContent = '▶ Resume';
      pauseBtn.classList.add('active');
    } else {
      pauseBtn.textContent = '⏸ Pause';
      pauseBtn.classList.remove('active');
      scrollTerminal();
    }
  };

  for (const cb of Object.values(filterCbs)) {
    if (cb) cb.addEventListener('change', applySourceFilters);
  }
  applySourceFilters();

  exportBtn.onclick = () => {
    // textContent includes filtered-out lines, so the log stays complete.
    const text = terminal.textContent;
    const blob = new Blob([text], { type: 'text/plain;charset=utf-8' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    const d = new Date().toISOString().replace(/[:.]/g, '-');
    a.download = `serial_console_${d}.txt`;
    a.click();
    URL.revokeObjectURL(a.href);
  };

  sendBtn.onclick = () => window.sendCommand();

  cmdInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      window.sendCommand();
    } else if (e.key === 'ArrowUp') {
      if (history.length === 0) return;
      e.preventDefault();
      if (historyIdx > 0) {
        historyIdx--;
        cmdInput.value = history[historyIdx];
      } else if (historyIdx === -1) {
        historyIdx = history.length - 1;
        cmdInput.value = history[historyIdx];
      }
    } else if (e.key === 'ArrowDown') {
      if (historyIdx >= 0) {
        e.preventDefault();
        if (historyIdx < history.length - 1) {
          historyIdx++;
          cmdInput.value = history[historyIdx];
        } else {
          historyIdx = history.length;
          cmdInput.value = '';
        }
      }
    }
  });

  connect();
})();
</script>
</body>
</html>
)HTML";
