#include "gateway.h"
#include <WebServer.h>
#include <WiFi.h>

namespace
{
WebServer server(80);
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#0c1c2e">
  <title>SMO Monitor</title>
  <style>
    :root { color-scheme: dark; --text:#edf4fa; --muted:#9eafc1; --line:#2c455d; --green:#54dfaa; --blue:#1c9ee3; }
    * { box-sizing:border-box; }
    body { margin:0; min-height:100vh; background:linear-gradient(155deg,#112c46 0%,#091827 50%,#060f1b 100%); color:var(--text); font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
    main { width:min(100%,760px); margin:auto; padding:38px 22px 48px; }
    header { display:flex; justify-content:space-between; align-items:flex-start; gap:14px; margin:0 4px 30px; }
    h1 { font-size:27px; line-height:1.2; letter-spacing:-.6px; margin:0 0 10px; }
    .meta { margin:0; color:var(--muted); font-size:13px; overflow-wrap:anywhere; }
    .server { text-align:center; flex-shrink:0; }
    .server-label { display:block; color:var(--muted); font-size:11px; margin-bottom:7px; }
    .badge { display:inline-block; padding:9px 14px; border:1px solid #38506a; border-radius:999px; font-size:12px; font-weight:800; letter-spacing:1px; color:var(--muted); background:#142a40; }
    .badge.online { color:var(--green); border-color:#28594f; background:#123731; }
    .badge.offline { color:#ffb6a9; border-color:#614345; background:#352c34; }
    .card { background:rgba(13,29,46,.9); border:1px solid var(--line); border-radius:22px; padding:24px; margin-bottom:20px; box-shadow:inset 0 1px 0 #ffffff04; }
    h2 { font-size:15px; font-weight:750; letter-spacing:1.8px; margin:0 0 22px; }
    fieldset { padding:0; margin:0; border:0; min-width:0; }
    .fields { display:grid; gap:18px; }
    label { display:block; color:var(--muted); font-size:15px; margin:0 0 7px; }
    input,select,button { font:inherit; }
    input,select { display:block; width:100%; height:50px; padding:11px 14px; border:1px solid #3a546b; border-radius:13px; background:#081825; color:var(--text); min-width:0; }
    input::placeholder { color:#657e93; }
    input:focus-visible,select:focus-visible,button:focus-visible { outline:2px solid #65c6ff; outline-offset:3px; }
    button { cursor:pointer; border:0; border-radius:13px; min-height:46px; font-weight:700; }
    .primary { background:linear-gradient(115deg,#258ddd,#16a6e5); color:white; padding:14px 23px; font-size:14px; margin-top:22px; }
    .wide { width:100%; }
    button:disabled { cursor:wait; opacity:.55; }
    fieldset:disabled { opacity:.6; }
    .hint { color:var(--muted); font-size:13px; margin:8px 0 20px; }
    .server-hint { color:var(--muted); font-size:13px; margin:0 0 20px; }
    .server-hint a { color:#70caff; overflow-wrap:anywhere; text-underline-offset:3px; }
    .server-hint a:focus-visible { outline:2px solid #65c6ff; outline-offset:3px; }
    .wifi-status { font-size:15px; font-weight:700; margin:0 0 6px; color:var(--muted); overflow-wrap:anywhere; }
    .status-grid { display:grid; grid-template-columns:minmax(0,1fr); gap:12px; margin:0; }
    .status-grid div { background:#10243a; border:1px solid var(--line); border-radius:12px; padding:12px; min-width:0; }
    .status-grid dt { font-size:12px; color:var(--muted); }
    .status-grid dd { margin:4px 0 0; font-size:16px; font-weight:700; overflow-wrap:anywhere; }
    .connected { color:var(--green); }
    .operation-grid { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
    .operation-grid button { padding:12px; color:white; background:#188dc6; }
    .operation-grid .stop { background:#d63d49; }
    .operation-grid .manual { grid-column:1/-1; background:#d99016; }
    .limit-toggle { display:flex; align-items:center; justify-content:space-between; gap:12px; margin-top:18px; cursor:pointer; }
    .limit-toggle input { position:absolute; width:1px; height:1px; padding:0; opacity:0; }
    .limit-switch { position:relative; flex:0 0 50px; width:50px; height:28px; border-radius:999px; background:#526477; transition:background .15s; }
    .limit-switch::after { content:""; position:absolute; top:3px; left:3px; width:22px; height:22px; border-radius:50%; background:#fff; box-shadow:0 1px 3px #0005; transition:transform .15s; }
    .limit-toggle input:checked + .limit-switch { background:#16a6e5; }
    .limit-toggle input:checked + .limit-switch::after { transform:translateX(22px); }
    .limit-toggle input:focus-visible + .limit-switch { outline:2px solid #65c6ff; outline-offset:3px; }
    .network-heading { display:flex; justify-content:space-between; align-items:center; margin-top:24px; }
    .network-heading label { margin:0; }
    .text-button { color:#70caff; background:none; padding:7px 0 7px 16px; min-height:40px; font-size:13px; }
    .password-wrap { position:relative; }
    .password-wrap input { padding-right:52px; }
    .eye { position:absolute; right:4px; top:3px; width:44px; min-height:44px; background:none; color:var(--muted); display:grid; place-items:center; }
    .eye svg { width:21px; height:21px; fill:none; stroke:currentColor; stroke-width:1.7; }
    .password-label { margin-top:18px; }
    .feedback { color:var(--muted); font-size:13px; margin:12px 0 0; overflow-wrap:anywhere; }
    .feedback:empty { display:none; }
    .feedback.success { color:var(--green); }
    .feedback.error { color:#ff6b7a; }
    #manual-network { margin-top:18px; }
    [hidden] { display:none !important; }
    @media(min-width:620px) { main { padding-top:52px; } h1 { font-size:32px; } .card { padding:28px; } .fields { grid-template-columns:1fr 1fr; gap:22px; } }
    @media(max-width:360px) { main { padding:28px 15px; } .card { padding:20px; } h1 { font-size:24px; } .badge { padding:8px 10px; } }
    @media(prefers-reduced-motion:no-preference) { button { transition:filter .15s; } button:hover:not(:disabled) { filter:brightness(1.1); } }
  </style>
</head>
<body>
<main>
  <header>
    <div><h1>SMO Monitor</h1><p class="meta" id="gateway-id"></p></div>
    <div class="server"><span class="server-label">Server connection</span><span id="server-status" class="badge" role="status" aria-live="polite">CHECKING</span></div>
  </header>
  <p class="server-hint">Save all test information and connect to Wi-Fi. Once ONLINE, open <a href="https://sklsmomonitor.web.app/" target="_blank" rel="noopener noreferrer">sklsmomonitor.web.app</a> to access the server.</p>
  <section class="card" aria-labelledby="info-title">
    <h2 id="info-title">TEST INFORMATION</h2>
    <form id="info-form">
      <fieldset id="info-fields" disabled>
        <div class="fields">
          <div><label for="device-name">Device name</label><input id="device-name" name="gateway_name" maxlength="120" placeholder="e.g. GW-..."></div>
          <div><label for="tester">Tester name</label><input id="tester" name="tester_name" maxlength="120" autocomplete="name" placeholder="e.g. Tester A"></div>
          <div><label for="location">Test location</label><input id="location" name="test_location" maxlength="120" placeholder="e.g. R&amp;D Lab 02 - Bay 4"></div>
          <div><label for="door">Door type</label><input id="door" name="door_type" maxlength="120" placeholder="e.g. Sectional Garage Door"></div>
          <div><label for="motor">Motor type</label><input id="motor" name="motor_type" maxlength="120" placeholder="e.g. BLDC 24V"></div>
        </div>
        <button class="primary" id="save-info" type="submit">Save information</button>
      </fieldset>
    </form>
    <p id="info-feedback" class="feedback" role="status">Loading saved information...</p>
    <button id="retry-info" class="text-button" type="button" hidden>Retry</button>
  </section>
  <section class="card">
    <h2>SERVER CONNECTION</h2>
    <form id="broker-form">
      <label for="broker-mode">Server</label>
      <select id="broker-mode"><option value="cloud">Cloud</option><option value="local">Local</option></select>
      <p>Stop the test before switching.</p>
      <button class="primary" id="broker-save" type="submit">Save and restart</button>
    </form>
    <p id="broker-feedback" class="feedback" role="status"></p>
  </section>
  <section class="card" aria-labelledby="wifi-title">
    <h2 id="wifi-title">WI-FI SETUP</h2>
    <p id="wifi-status" class="wifi-status" role="status">Checking Wi-Fi connection...</p>
    <p class="hint" id="connection-hint">Select a Wi-Fi network and enter its password to connect this gateway.</p>
    <form id="wifi-form">
      <div class="network-heading"><label for="networks">Wi-Fi network</label><button id="scan" class="text-button" type="button">Scan again</button></div>
      <select id="networks" required><option value="">Select a network</option><option value="manual">Enter network manually...</option></select>
      <div id="manual-network" hidden><label for="ssid">Network name (SSID)</label><input id="ssid" maxlength="32" autocomplete="off" placeholder="Enter network name"></div>
      <label class="password-label" for="password">Password</label>
      <div class="password-wrap"><input id="password" type="password" maxlength="63" autocomplete="new-password" aria-describedby="password-hint"><button class="eye" id="show-password" type="button" aria-label="Show password" aria-pressed="false"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6-10-6-10-6Z"/><circle cx="12" cy="12" r="3"/></svg></button></div>
      <p class="hint" id="password-hint">Leave blank for an open network.</p>
      <button class="primary wide" id="connect" type="submit">Connect</button>
    </form>
    <p id="wifi-feedback" class="feedback" role="status"></p>
  </section>
  <section class="card" aria-labelledby="button-title">
    <h2 id="button-title">GATEWAY SETTINGS</h2>
    <form id="gateway-config">
      <fieldset id="gateway-config-fields" disabled>
        <label for="cycle-sec">Cycle interval (seconds)</label>
        <input id="cycle-sec" name="cycle_sec" type="number" min="1" max="86400" step="1" required>
        <label class="limit-toggle"><span>Limit button presses</span><input id="limit-enabled" type="checkbox" role="switch"><span class="limit-switch" aria-hidden="true"></span></label>
        <div id="limit-wrap" hidden><label for="press-limit">Target button presses</label><input id="press-limit" type="number" min="1" max="1000000" step="1"></div>
        <button class="primary wide" type="submit">Save config</button>
      </fieldset>
    </form>
    <p id="config-feedback" class="feedback" role="status"></p>
    <p id="config-sync" class="feedback" role="status"></p>
  </section>
  <section class="card" aria-labelledby="operation-title">
    <h2 id="operation-title">GATEWAY OPERATION CONTROL</h2>
    <dl class="status-grid">
      <div><dt>Test state</dt><dd id="button-state">Checking...</dd></div>
      <div><dt>Button presses / target</dt><dd id="button-count">—</dd></div>
      <div><dt>Elapsed time (includes pauses)</dt><dd id="button-elapsed">00:00:00</dd></div>
    </dl>
    <div class="operation-grid" style="margin-top:18px">
      <button id="test-action" type="button" disabled>Start test</button>
      <button id="test-stop" class="stop" type="button" disabled>Stop test</button>
      <button id="test-manual" class="manual" type="button" disabled>Manual trigger</button>
    </div>
    <p id="control-feedback" class="feedback" role="status"></p>
  </section>
</main>
<script>
const $ = id => document.getElementById(id);
let latestStatus = null;
let configDirty = false, configBusy = false, controlBusy = false;
let brokerLoaded = false, brokerSwitching = false;
$('broker-form').onsubmit = async event => {
  event.preventDefault();
  if (brokerSwitching) return;
  const mode = $('broker-mode').value;
  brokerSwitching = true;
  $('broker-mode').disabled = true;
  $('broker-save').disabled = true;
  feedback('broker-feedback',`Switching to ${mode === 'local' ? 'Local' : 'Cloud'}...`);
  try {
    await request('/api/mqtt/server', new URLSearchParams({mode}));
    brokerLoaded = true;
    feedback('broker-feedback',`Switching to ${mode === 'local' ? 'Local' : 'Cloud'}. Gateway is restarting; reconnect to its Wi-Fi and reload this page.`,'success');
  } catch(error) {
    brokerSwitching = false;
    $('broker-mode').disabled = false;
    $('broker-save').disabled = false;
    feedback('broker-feedback',error.message,'error');
  }
};
function renderControls(status) {
  const state = status && status.run_state;
  $('test-action').textContent = state === 'running' ? 'Pause test' : state === 'paused' ? 'Resume test' : 'Start test';
  $('test-action').disabled = !status || controlBusy;
  $('test-stop').disabled = !status || controlBusy || !['running','paused'].includes(state);
  $('test-manual').disabled = !status || controlBusy || !!status.button_pulse_active;
  $('gateway-config-fields').disabled = !status || configBusy;
}
function renderGateway(status) {
  renderControls(status);
  $('button-state').textContent = status.run_state || 'unknown';
  $('button-count').textContent = `${status.button_count} / ${status.button_limit || 'Unlimited'}`;
  const s = status.elapsed_sec || 0;
  $('button-elapsed').textContent = [Math.floor(s/3600),Math.floor(s/60)%60,s%60].map(v=>String(v).padStart(2,'0')).join(':');
  if (!configDirty && !configBusy) {
    $('cycle-sec').value = status.cycle_sec || '';
    $('limit-enabled').checked = status.button_limit > 0;
    $('press-limit').value = status.button_limit || '';
    updateLimit();
  }
  $('config-sync').textContent = status.local_config_pending ? 'Configuration saved locally; waiting for server sync.' : '';
}
function updateLimit() {
  $('limit-wrap').hidden = !$('limit-enabled').checked;
  $('press-limit').required = $('limit-enabled').checked;
  $('press-limit').disabled = !$('limit-enabled').checked;
}
$('gateway-config').oninput = () => { configDirty = true; updateLimit(); };
$('gateway-config').onsubmit = async event => {
  event.preventDefault();
  const body = new URLSearchParams({cycle_sec:$('cycle-sec').value,limit:$('limit-enabled').checked ? $('press-limit').value : '0'});
  configBusy = true; renderControls(latestStatus);
  try {
    const message = await request('/api/gateway/config',body);
    configDirty = false;
    feedback('config-feedback',message,'success');
    latestStatus = await request('/api/status');
  } catch(error) { feedback('config-feedback',error.message,'error'); }
  finally { configBusy = false; if(latestStatus) renderGateway(latestStatus); else renderControls(null); }
};
async function control(action) {
  if(controlBusy) return;
  controlBusy = true; renderControls(latestStatus);
  try {
    const message = await request('/api/gateway/control',new URLSearchParams({action}));
    feedback('control-feedback',message,'success');
    latestStatus = await request('/api/status');
  } catch(error) { feedback('control-feedback',error.message,'error'); }
  finally { controlBusy = false; if(latestStatus) renderGateway(latestStatus); else renderControls(null); }
}
$('test-action').onclick = () => control(latestStatus?.run_state === 'running' ? 'PAUSE_TEST' : latestStatus?.run_state === 'paused' ? 'RESUME_TEST' : 'START_TEST');
$('test-stop').onclick = () => control('STOP_TEST');
$('test-manual').onclick = () => control('TRIGGER_MANUAL');
let scanning = false;
let connecting = null;
let networkEdited = false;
const byteLength = text => new TextEncoder().encode(text).length;
const feedbackTimers = new Map();
function feedback(id, text, tone = '') {
  clearTimeout(feedbackTimers.get(id));
  feedbackTimers.delete(id);
  $(id).textContent = text;
  $(id).className = 'feedback' + (tone ? ' ' + tone : '');
  if (text) {
    feedbackTimers.set(id, setTimeout(() => {
      $(id).textContent = '';
      $(id).className = 'feedback';
      feedbackTimers.delete(id);
    }, 10000));
  }
}
async function request(path, body, headers = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 8000);
  try {
    const response = await fetch(path, {method: body ? 'POST' : 'GET', body, headers, cache:'no-store', signal:controller.signal});
    if (!response.ok) throw new Error(await response.text());
    return body ? response.text() : response.json();
  } catch (error) {
    if (error.name === 'AbortError' || error instanceof TypeError) throw new Error('Gateway unavailable. Reconnect to its Wi-Fi and try again.');
    throw error;
  } finally { clearTimeout(timer); }
}
async function loadInfo() {
  $('retry-info').hidden = true;
  feedback('info-feedback', 'Loading saved information...');
  try {
    const info = await request('/api/info');

    for (const name of ['gateway_name','tester_name','test_location','door_type','motor_type']) $('info-form').elements[name].value = info[name] || '';
    $('info-fields').disabled = false;
    feedback('info-feedback', '');
  } catch (error) {
    feedback('info-feedback', error.message, 'error');
    $('retry-info').hidden = false;
  }
}
$('retry-info').onclick = loadInfo;
$('info-form').onsubmit = async event => {
  event.preventDefault();
  const body = new URLSearchParams(new FormData(event.target));
  for (const name of ['gateway_name','tester_name','test_location','door_type','motor_type']) {
    const value = (body.get(name) || '').trim();
    if (!value && name !== 'gateway_name') {
      feedback('info-feedback', 'All test information fields are required', 'error');
      return;
    }
    body.set(name, value);
  }
  for (const value of body.values()) {
    if (byteLength(value) > 120) { feedback('info-feedback', 'Each field must be 120 bytes or fewer.', 'error'); return; }
  }
  $('save-info').disabled = true;
  feedback('info-feedback', 'Saving information...');
  try {
    const message = await request('/api/info', body);
    feedback('info-feedback', message, 'success');
  }
  catch (error) { feedback('info-feedback', error.message, 'error'); }
  finally { $('save-info').disabled = false; }
};
function ensureConnectedOption(status) {
  if (!status.wifi_connected || networkEdited || $('networks').value === 'manual') return;
  let option = Array.from($('networks').options).find(item => item.dataset.ssid === status.ssid);
  if (!option) {
    option = new Option(status.ssid + ' (connected)', 'connected');
    option.dataset.ssid = status.ssid;
    $('networks').add(option, $('networks').options.length - 1);
  }
  $('networks').value = option.value;
}
async function refreshStatus() {

  try {
    const status = await request('/api/status');

    latestStatus = status;
    renderGateway(status);
    $('device-name').placeholder = `e.g. ${status.gateway_id}`;
    $('gateway-id').textContent = status.gateway_id;
    if (!brokerLoaded && !brokerSwitching) {
      $('broker-mode').value = status.mqtt_server || 'cloud';
      brokerLoaded = true;
    }
    $('server-status').textContent = status.mqtt_connected ? 'ONLINE' : 'OFFLINE';
    $('server-status').className = 'badge ' + (status.mqtt_connected ? 'online' : 'offline');
    $('button-state').textContent = status.run_state || 'unknown';
    $('wifi-status').className = 'wifi-status' + (status.wifi_connected ? ' connected' : '');
    $('wifi-status').textContent = status.wifi_connected
      ? `Connected to ${status.ssid} \u00b7 ${status.ip}` + (typeof status.rssi_dbm === 'number' ? ` \u00b7 ${status.rssi_dbm} dBm` : '')
      : 'Not connected to Wi-Fi';
    $('connection-hint').textContent = status.wifi_connected
      ? 'To configure another Wi-Fi network, select it and enter its password below.'
      : 'Select a Wi-Fi network and enter its password to connect this gateway.';
    ensureConnectedOption(status);
    if (connecting) {
      if (status.wifi_connected && status.ssid === connecting.ssid) {
        feedback('wifi-feedback', 'Wi-Fi connected successfully.', 'success');
        connecting = null; $('connect').disabled = false; $('connect').textContent = 'Connect';
      } else if (Date.now() - connecting.started >= 10000) {
        feedback('wifi-feedback', 'Connection not confirmed. Check the network and password, then try again.', 'error');
        connecting = null; $('connect').disabled = false; $('connect').textContent = 'Connect';
      }
    }
  } catch (error) {

    latestStatus = null;
    renderControls(null);
    $('server-status').textContent = 'UNKNOWN';
    $('server-status').className = 'badge';
    $('button-state').textContent = 'Unavailable';
    $('wifi-status').className = 'wifi-status';
    $('wifi-status').textContent = 'Gateway unavailable';
    $('connection-hint').textContent = 'Reconnect to the gateway Wi-Fi to view its connection status.';
    if (connecting && Date.now() - connecting.started >= 10000) {
      connecting = null; $('connect').disabled = false; $('connect').textContent = 'Connect';
      feedback('wifi-feedback', 'Connection not confirmed. Reconnect to the gateway Wi-Fi to check.', 'error');
    }
  }
  setTimeout(refreshStatus, 2000);
}
$('networks').onchange = () => {
  networkEdited = true;
  const manual = $('networks').value === 'manual';
  $('manual-network').hidden = !manual;
  $('ssid').required = manual;
  if (manual) $('ssid').focus();
};
$('show-password').onclick = () => {
  const show = $('password').type === 'password';
  $('password').type = show ? 'text' : 'password';
  $('show-password').setAttribute('aria-label', show ? 'Hide password' : 'Show password');
  $('show-password').setAttribute('aria-pressed', String(show));
};
async function scanNetworks() {
  if (scanning || connecting) return;
  scanning = true; $('connect').disabled = true; $('scan').disabled = true; $('scan').textContent = 'Scanning...';
  feedback('wifi-feedback', 'Scanning for Wi-Fi networks...');
  try {
    let result;
    for (let i = 0; i < 20; i++) {
      result = await request('/api/wifi/scan');

      if (!result.scanning) break;
      await new Promise(resolve => setTimeout(resolve, 700));
    }
    if (result.scanning) throw new Error('Wi-Fi scan timed out. Please try again.');
    const selected = $('networks').selectedOptions[0];
    const selectedSsid = selected ? selected.dataset.ssid : '';
    const manual = $('networks').value === 'manual';
    const options = [new Option('Select a network', '')];
    const unique = new Map();
    for (const item of result.networks || []) {
      if (item.ssid && (!unique.has(item.ssid) || unique.get(item.ssid).rssi < item.rssi)) unique.set(item.ssid, item);
    }
    for (const item of Array.from(unique.values()).sort((a,b) => b.rssi-a.rssi)) {
      const option = new Option(`${item.ssid} \u00b7 ${item.rssi} dBm` + (item.open ? ' (open)' : ''), 'network-' + options.length);
      option.dataset.ssid = item.ssid;
      option.selected = !manual && item.ssid === selectedSsid;
      options.push(option);
    }
    if (selectedSsid && !unique.has(selectedSsid)) {
      const option = new Option(selectedSsid, 'previous'); option.dataset.ssid = selectedSsid; option.selected = !manual; options.push(option);
    }
    options.push(new Option('Enter network manually...', 'manual', false, manual));
    $('networks').replaceChildren(...options);
    if (latestStatus) ensureConnectedOption(latestStatus);
    feedback('wifi-feedback', unique.size ? 'Select a network to connect.' : 'No networks found. Scan again or enter a network manually.');
  } catch (error) { feedback('wifi-feedback', error.message, 'error'); }
  finally { scanning = false; $('connect').disabled = !!connecting; $('scan').disabled = false; $('scan').textContent = 'Scan again'; }
}
$('scan').onclick = scanNetworks;
$('wifi-form').onsubmit = async event => {
  event.preventDefault();
  const selection = $('networks').selectedOptions[0];
  const ssid = $('networks').value === 'manual' ? $('ssid').value : (selection && selection.dataset.ssid) || '';
  const password = $('password').value;
  if (!ssid || byteLength(ssid) > 32) { feedback('wifi-feedback', 'Select a network or enter a valid network name (up to 32 bytes).', 'error'); return; }
  if (password && (byteLength(password) < 8 || byteLength(password) > 63)) { feedback('wifi-feedback', 'The Wi-Fi password must be 8 to 63 bytes, or blank for an open network.', 'error'); return; }
  $('connect').disabled = true; $('connect').textContent = 'Connecting...';
  feedback('wifi-feedback', 'Saving Wi-Fi settings...');
  try {
    const message = await request('/api/wifi', new URLSearchParams({ssid, password}));

    connecting = {ssid, started: Date.now()};
    feedback('wifi-feedback', message);
  } catch (error) {
    feedback('wifi-feedback', error.message, 'error');
    $('connect').disabled = false; $('connect').textContent = 'Connect';
  }
};
loadInfo();
refreshStatus();
scanNetworks();
</script>
</body></html>)HTML";

void sendJson(const JsonDocument &document)
{
    String response;
    serializeJson(document, response);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", response);
}
bool unsignedField(const String &value, uint32_t maximum, uint32_t &result)
{
    if (value.isEmpty() || value.length() > 10) return false;
    result = 0;
    for (unsigned i = 0; i < value.length(); ++i)
    {
        char c = value[i];
        if (c < '0' || c > '9') return false;
        unsigned digit = c - '0';
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10)) return false;
        result = result * 10 + digit;
    }
    return true;
}
} // namespace

void webBegin()
{
    server.on("/api/mqtt/server", HTTP_POST, [] {
        const char *error = gatewaySelectBroker(server.arg("mode"));
        server.send(error ? 400 : 200, "text/plain", error ? error : "Server saved. Gateway will restart; reconnect to its Wi-Fi if needed.");
    });
    server.on("/api/gateway/config", HTTP_POST, [] {
        uint32_t cycle, limit;
        if (!unsignedField(server.arg("cycle_sec"), 86400, cycle) || cycle == 0 ||
            !unsignedField(server.arg("limit"), 1000000, limit))
        {
            server.send(400, "text/plain", "Cycle must be 1-86400 seconds; limit must be 0-1000000.");
            return;
        }
        const char *error = gatewayLocalConfig(cycle, limit);
        server.send(error ? 409 : 200, "text/plain", error ? error : "Configuration saved.");
    });
    server.on("/api/gateway/control", HTTP_POST, [] {
        const char *error = gatewayLocalControl(server.arg("action"));
        server.send(error ? 409 : 200, "text/plain", error ? error : "Command applied.");
    });
    server.on("/", HTTP_GET,
              []
              {
                  server.sendHeader("Cache-Control", "no-store");
                  server.send_P(200, "text/html; charset=utf-8", PAGE);
              });
    server.on("/api/status", HTTP_GET,
              []
              {
                  JsonDocument d;
                  gatewayStatus(d);
                  sendJson(d);
              });
    server.on("/api/uart", HTTP_GET,
              [] { JsonDocument d; gatewayUartStatus(d); sendJson(d); });
    server.on("/api/info", HTTP_GET,
              []
              {
                  JsonDocument d;
                  gatewayInfo(d);
                  sendJson(d);
              });
    server.on("/api/errors", HTTP_GET,
              [] { server.send(200, "application/json", gatewayHistory()); });
    server.on("/api/info", HTTP_POST,
              []
              {
                  String tester = server.arg("tester_name"), location = server.arg("test_location");
                  String door = server.arg("door_type"), motor = server.arg("motor_type");
                  String name = server.hasArg("gateway_name") ? server.arg("gateway_name") : server.arg("device_name");
                  name.trim();
                  tester.trim();
                  location.trim();
                  door.trim();
                  motor.trim();
                  if (tester.isEmpty() || location.isEmpty() || door.isEmpty() || motor.isEmpty())
                  {
                      server.send(400, "text/plain", "All test information fields are required");
                      return;
                  }
                  if (tester.length() > 120 || location.length() > 120 || door.length() > 120 ||
                      motor.length() > 120 || name.length() > 120)
                  {
                      server.send(400, "text/plain", "Each field must be 120 bytes or fewer.");
                      return;
                  }
                  if (!gatewaySaveInfo(tester, location, door, motor, name))
                  {
                      server.send(500, "text/plain",
                                  "Could not save test information. Please retry.");
                      return;
                  }
                  server.send(200, "text/plain", "Test information saved");
              });
    server.on("/api/wifi", HTTP_POST,
              []
              {
                  String ssid = server.arg("ssid"), password = server.arg("password");
                  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
                      (!password.isEmpty() && password.length() < 8))
                  {
                      server.send(400, "text/plain", "Invalid network name or password.");
                      return;
                  }
                  gatewaySaveWifi(ssid, password);
                  server.send(200, "text/plain", "Wi-Fi settings saved. Connecting...");
              });
    server.on("/api/wifi/scan", HTTP_GET,
              []
              {
                  static uint32_t scanStart = 0;
                  JsonDocument d;
                  auto networks = d["networks"].to<JsonArray>();
                  int count = WiFi.scanComplete();
                  if (count == WIFI_SCAN_RUNNING && millis() - scanStart > 12000)
                  {
                      WiFi.scanDelete();
                      server.send(503, "text/plain", "Wi-Fi scan timed out. Please try again.");
                      return;
                  }
                  if (count == WIFI_SCAN_FAILED)
                  {
                      if (WiFi.status() != WL_CONNECTED)
                          WiFi.disconnect(false, false);
                      int result = WiFi.scanNetworks(true);
                      scanStart = millis();
                      if (result == WIFI_SCAN_FAILED)
                      {
                          server.send(503, "text/plain", "Wi-Fi scan failed. Please try again.");
                          return;
                      }
                      d["scanning"] = true;
                  }
                  else if (count == WIFI_SCAN_RUNNING)
                  {
                      d["scanning"] = true;
                  }
                  else
                  {
                      d["scanning"] = false;
                      for (int i = 0; i < count && i < 30; i++)
                      {
                          auto network = networks.add<JsonObject>();
                          network["ssid"] = WiFi.SSID(i);
                          network["rssi"] = WiFi.RSSI(i);
                          network["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
                      }
                      WiFi.scanDelete();
                  }
                  sendJson(d);
              });
    server.begin();
}
void webLoop()
{
    gatewayWebLoopTick();
    server.handleClient();
}
