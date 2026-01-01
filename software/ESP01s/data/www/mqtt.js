const serverInput = document.getElementById('mqtt-server');
const portInput = document.getElementById('mqtt-port');
const userInput = document.getElementById('mqtt-user');
const passInput = document.getElementById('mqtt-pass');
const statusEl = document.getElementById('mqtt-status');
const form = document.getElementById('mqtt-form');
const chipIdEl = document.getElementById('chip-id');
const topicStateEl = document.getElementById('topic-state');
const topicAvailabilityEl = document.getElementById('topic-availability');
const topicDiscVoltageEl = document.getElementById('topic-disc-voltage');
const topicDiscCurrentEl = document.getElementById('topic-disc-current');
const topicDiscPowerEl = document.getElementById('topic-disc-power');
const mqttLastLogEl = document.getElementById('mqtt-last-log');

async function loadConfig() {
  statusEl.textContent = 'Loading...';
  try {
    const r = await fetch('/api/mqtt/config', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.statusText);
    const j = await r.json();
    serverInput.value = j.server || '';
    portInput.value = j.port || '';
    userInput.value = j.user || '';
    passInput.value = j.pass || '';
    statusEl.textContent = 'Loaded';
  } catch (e) {
    statusEl.textContent = 'Load failed';
  }
}

async function saveConfig(ev) {
  ev.preventDefault();
  statusEl.textContent = 'Saving...';
  const payload = {
    server: serverInput.value.trim(),
    port: Number(portInput.value || 0),
    user: userInput.value.trim(),
    pass: passInput.value,
  };

  try {
    const r = await fetch('/api/mqtt/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!r.ok) {
      const t = await r.text();
      throw new Error(t || r.statusText);
    }
    statusEl.textContent = 'Saved';
  } catch (e) {
    statusEl.textContent = 'Save failed';
  }
}

form.addEventListener('submit', saveConfig);
loadConfig();

async function loadDeviceInfo() {
  try {
    const r = await fetch('/api/device/info', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.statusText);
    const j = await r.json();
    const chipId = j.chipId || '';
    if (!chipId) return;

    const base = `pd_logger/${chipId}`;
    const deviceId = `pd_logger_${chipId}`;

    chipIdEl.textContent = chipId;
    topicStateEl.textContent = `${base}/state`;
    topicAvailabilityEl.textContent = `${base}/availability`;
    topicDiscVoltageEl.textContent = `homeassistant/sensor/${deviceId}_voltage/config`;
    topicDiscCurrentEl.textContent = `homeassistant/sensor/${deviceId}_current/config`;
    topicDiscPowerEl.textContent = `homeassistant/sensor/${deviceId}_power/config`;
  } catch (e) {
    chipIdEl.textContent = '—';
  }
}

loadDeviceInfo();

async function loadMqttStatus() {
  try {
    const r = await fetch('/api/mqtt/status', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.statusText);
    const j = await r.json();
    mqttLastLogEl.textContent = j.message || '—';
  } catch (e) {
    mqttLastLogEl.textContent = '—';
  }
}

setInterval(loadMqttStatus, 2000);
loadMqttStatus();
