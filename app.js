const mqttConfig = {
  host: "wss://u660b616.ala.eu-central-1.emqxsl.com:8084/mqtt",
  username: "helmet123",
  password: "helmet123",
  clientId: `sand76-web-${Math.random().toString(16).slice(2, 8)}`
};

const topicMap = {
  key: "lock/status/key",
  nfc: "lock/status/nfc",
  fingerprint: "lock/status/fingerprint",
  vein: "lock/status/vein",
  relay: "lock/status/relay",
  system: "lock/status/system",
  enroll: "lock/enroll/progress",
  logEntry: "lock/log/entry",
  logLast: "lock/log/last"
};

const state = {
  key: { ok: false, text: "Waiting", ts: null },
  nfc: { ok: false, text: "Waiting", ts: null },
  fingerprint: { ok: false, text: "Waiting", ts: null },
  vein: { ok: false, text: "Waiting", ts: null },
  relay: { ok: false, text: "Locked", ts: null },
  system: { ok: false, text: "Waiting for data", ts: null }
};

const storageKey = "vit-smart-lock-logs";
const logList = document.getElementById("logList");
const connectionPill = document.getElementById("connectionPill");
const overallState = document.getElementById("overallState");
const relayText = document.getElementById("relayText");
const systemMessage = document.getElementById("systemMessage");
const latestEvent = document.getElementById("latestEvent");
const latestEventTime = document.getElementById("latestEventTime");
const enrollState = document.getElementById("enrollState");
const enrollDetail = document.getElementById("enrollDetail");
const panels = [...document.querySelectorAll(".panel")];
const menuItems = [...document.querySelectorAll(".menu-item")];
const sidebar = document.getElementById("sidebar");
let lastLogSignature = "";

function nowLabel(ts) {
  if (!ts) return "No update yet";
  const value = new Date(ts);
  if (Number.isNaN(value.getTime())) return String(ts);
  return value.toLocaleString();
}

function parsePayload(message) {
  const text = message.toString();
  try {
    return JSON.parse(text);
  } catch {
    return { text, ok: /ok|success|unlock|matched|active|on/i.test(text), ts: new Date().toISOString() };
  }
}

function updateOverall() {
  const allGreen = state.key.ok && state.nfc.ok && state.fingerprint.ok && state.vein.ok;
  overallState.textContent = allGreen ? "UNLOCKED" : "LOCKED";
  overallState.style.background = allGreen ? "var(--green-soft)" : "var(--red-soft)";
  overallState.style.color = allGreen ? "var(--green)" : "var(--red)";
}

function setCardStatus(name, payload) {
  state[name] = {
    ok: Boolean(payload.ok),
    text: payload.text || payload.status || "Waiting",
    ts: payload.ts || new Date().toISOString()
  };

  const card = document.getElementById(`card-${name}`);
  document.getElementById(`value-${name}`).textContent = state[name].text;
  document.getElementById(`time-${name}`).textContent = nowLabel(state[name].ts);
  card.classList.remove("status-red", "status-green");
  card.classList.add(state[name].ok ? "status-green" : "status-red");
  updateOverall();
}

function renderLogs() {
  const logs = JSON.parse(localStorage.getItem(storageKey) || "[]");
  if (!logs.length) {
    logList.innerHTML = '<div class="log-empty">No log entries yet.</div>';
    return;
  }

  logList.innerHTML = logs.slice().reverse().map((entry) => `
    <article class="log-entry">
      <div class="log-head">
        <span>${entry.title}</span>
        <span>${nowLabel(entry.ts)}</span>
      </div>
      <div>${entry.message}</div>
    </article>
  `).join("");
}

function pushLog(title, message, ts = new Date().toISOString()) {
  const signature = `${title}|${message}|${ts}`;
  if (signature === lastLogSignature) {
    return;
  }
  lastLogSignature = signature;

  const logs = JSON.parse(localStorage.getItem(storageKey) || "[]");
  logs.push({ title, message, ts });
  localStorage.setItem(storageKey, JSON.stringify(logs.slice(-200)));
  latestEvent.textContent = title;
  latestEventTime.textContent = `${message} • ${nowLabel(ts)}`;
  renderLogs();
}

function handleMessage(topic, message) {
  const payload = parsePayload(message);

  if (topic === topicMap.key) setCardStatus("key", payload);
  if (topic === topicMap.nfc) setCardStatus("nfc", payload);
  if (topic === topicMap.fingerprint) setCardStatus("fingerprint", payload);
  if (topic === topicMap.vein) setCardStatus("vein", payload);
  if (topic === topicMap.relay) {
    state.relay = payload;
    relayText.textContent = payload.text || "Locked";
  }
  if (topic === topicMap.system) {
    state.system = payload;
    systemMessage.textContent = payload.text || "System update received";
  }
  if (topic === topicMap.enroll) {
    enrollState.textContent = payload.text || "Enrollment update";
    enrollDetail.textContent = payload.detail || "Progress received from ESP32";
    pushLog("Enrollment", `${payload.text || "Update"}${payload.detail ? ` - ${payload.detail}` : ""}`, payload.ts);
  }
  if (topic === topicMap.logEntry || topic === topicMap.logLast) {
    pushLog(payload.title || "System Event", payload.message || payload.text || "Log update", payload.ts);
  }
  updateOverall();
}

function setConnectionState(text, connected) {
  connectionPill.textContent = text;
  connectionPill.style.background = connected ? "var(--green-soft)" : "rgba(18, 33, 47, 0.08)";
  connectionPill.style.color = connected ? "var(--green)" : "var(--ink)";
}

function activatePanel(id) {
  panels.forEach((panel) => panel.classList.toggle("active", panel.id === id));
  menuItems.forEach((item) => item.classList.toggle("active", item.dataset.target === id));
  sidebar.classList.remove("open");
}

menuItems.forEach((item) => item.addEventListener("click", () => activatePanel(item.dataset.target)));
document.getElementById("menuToggle").addEventListener("click", () => sidebar.classList.toggle("open"));
document.getElementById("clearLogs").addEventListener("click", () => {
  localStorage.removeItem(storageKey);
  renderLogs();
});

document.getElementById("enrollButton").addEventListener("click", () => {
  if (!window.dashboardClient || !window.dashboardClient.connected) {
    enrollState.textContent = "MQTT Offline";
    enrollDetail.textContent = "Connect to EMQX first, then try again.";
    return;
  }
  window.dashboardClient.publish("lock/command/enroll", JSON.stringify({
    command: "start",
    requestedBy: "web-dashboard",
    ts: new Date().toISOString()
  }), { qos: 1 });
  enrollState.textContent = "Command Sent";
  enrollDetail.textContent = "Waiting for ESP32 enrollment progress...";
});

renderLogs();
updateOverall();

window.dashboardClient = mqtt.connect(mqttConfig.host, {
  username: mqttConfig.username,
  password: mqttConfig.password,
  clientId: mqttConfig.clientId,
  clean: true,
  reconnectPeriod: 3000,
  connectTimeout: 30000
});

window.dashboardClient.on("connect", () => {
  setConnectionState("MQTT Connected", true);
  window.dashboardClient.subscribe("lock/#", { qos: 1 });
});

window.dashboardClient.on("reconnect", () => setConnectionState("Reconnecting...", false));
window.dashboardClient.on("offline", () => setConnectionState("Offline", false));
window.dashboardClient.on("error", () => setConnectionState("Connection Error", false));
window.dashboardClient.on("message", handleMessage);
