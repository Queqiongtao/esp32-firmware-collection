import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import "./style.css";

type CaptureSource = { id: string; kind: string; label: string; width: number; height: number };
type StreamEvent = { state: string; detail: string; fps: number; jpegBytes: number; droppedFrames: number; videoTransport: string; captureEncodeMs: number; captureWaitMs: number; deviceMs: number; deviceReplacedFrames: number };
type UdpDiagnostics = { rxPackets: number; acceptedFragments: number; rejectedPackets: number; completedFrames: number; submittedFrames: number; submitFailures: number; reassemblyTimeouts: number; lastReject: string };
type Performance = { sequence: number; queueWaitUs: number; jpegDecodeUs: number; lcdWaitUs: number; lcdSubmitUs: number; previousDmaUs: number; replacedFrames: number };
type DeviceStatus = { name: string; width: number; height: number; wifi: string; ipv4: string; activeTransport: string; tokenSet: boolean; protocolVersion: number; videoTransports: string[]; droppedFrames: number; performance: Performance; udp: UdpDiagnostics };

const byId = <T extends HTMLElement>(id: string) => document.querySelector<T>(`#${id}`)!;

const endpoint = byId<HTMLInputElement>("endpoint");
const token = byId<HTMLInputElement>("token");
const source = byId<HTMLSelectElement>("source");
const sourceSummary = byId<HTMLDivElement>("source-summary");
const status = byId<HTMLParagraphElement>("status");
const devicePill = byId<HTMLSpanElement>("device-pill");
const streamPill = byId<HTMLSpanElement>("stream-pill");
const readyLabel = byId<HTMLSpanElement>("ready-label");
const start = byId<HTMLButtonElement>("start");
const stop = byId<HTMLButtonElement>("stop");
const checkDevice = byId<HTMLButtonElement>("check-device");
const refresh = byId<HTMLButtonElement>("refresh");
const deviceStatus = byId<HTMLElement>("device-status");
const deviceWifi = byId<HTMLElement>("device-wifi");
const deviceScreen = byId<HTMLElement>("device-screen");
const deviceTransport = byId<HTMLElement>("device-transport");
const cropEnabled = byId<HTMLInputElement>("crop-enabled");
const cropFields = byId<HTMLDivElement>("crop-fields");
const regionX = byId<HTMLInputElement>("region-x");
const regionY = byId<HTMLInputElement>("region-y");
const regionWidth = byId<HTMLInputElement>("region-width");
const regionHeight = byId<HTMLInputElement>("region-height");
const fps = byId<HTMLInputElement>("fps");
const quality = byId<HTMLInputElement>("quality");
const videoTransport = byId<HTMLSelectElement>("video-transport");
const preview = byId<HTMLImageElement>("preview");
const previewEmpty = byId<HTMLSpanElement>("preview-empty");
const metricTransport = byId<HTMLElement>("m-transport");
const metricFps = byId<HTMLElement>("m-fps");
const metricCapture = byId<HTMLElement>("m-capture");
const metricEncode = byId<HTMLElement>("m-encode");
const metricDevice = byId<HTMLElement>("m-device");
const metricReplaced = byId<HTMLElement>("m-replaced");
const metricDropped = byId<HTMLElement>("m-dropped");
const metricLimited = byId<HTMLElement>("m-limited");

const SETTINGS_KEY = "companion-display.settings.v1";
const SLOW_STAGE_MS = 40;
const PROBE_DEBOUNCE_MS = 800;
const PARAMS_DEBOUNCE_MS = 300;
const DEVICE_POLL_MS = 2000;

const sourcesById = new Map<string, CaptureSource>();
let lastDevice: DeviceStatus | null = null;
let streamActive = false;
let streamStopping = false;
let stoppingStatusPoll: number | null = null;
let probeTimer: number | null = null;
let paramsTimer: number | null = null;
let lastDevicePoll = 0;

function setPill(element: HTMLElement, text: string, tone: "muted" | "good" | "warn" | "bad" | "active") {
  element.textContent = text;
  element.className = `pill ${tone}`;
}

function setStatus(text: string, tone: "normal" | "good" | "warn" | "bad" = "normal") {
  status.textContent = text;
  status.dataset.tone = tone;
}

function normalizeEndpoint(value: string) {
  const trimmed = value.trim();
  if (!trimmed) return "";
  const withScheme = trimmed.startsWith("ws://") ? trimmed : `ws://${trimmed}`;
  const withoutTrailingSlash = withScheme.endsWith("/") ? withScheme.slice(0, -1) : withScheme;
  return withoutTrailingSlash.includes("/stream") ? withoutTrailingSlash : `${withoutTrailingSlash}/stream`;
}

function sanitizeToken() {
  token.value = token.value.replace(/\D/g, "").slice(0, 6);
}

function saveSettings() {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify({
      endpoint: endpoint.value,
      token: token.value,
      fps: fps.value,
      quality: quality.value,
      videoTransport: videoTransport.value,
      cropEnabled: cropEnabled.checked,
      regionX: regionX.value,
      regionY: regionY.value,
      regionWidth: regionWidth.value,
      regionHeight: regionHeight.value,
    }));
  } catch {
    /* Private mode or blocked storage never blocks streaming. */
  }
}

function restoreSettings() {
  let raw: string | null = null;
  try {
    raw = localStorage.getItem(SETTINGS_KEY);
  } catch {
    return;
  }
  if (!raw) return;
  try {
    const saved = JSON.parse(raw) as Record<string, unknown>;
    if (typeof saved.endpoint === "string") endpoint.value = saved.endpoint;
    if (typeof saved.token === "string") token.value = saved.token;
    if (typeof saved.fps === "string") fps.value = saved.fps;
    if (typeof saved.quality === "string") quality.value = saved.quality;
    if (typeof saved.videoTransport === "string") videoTransport.value = saved.videoTransport;
    if (typeof saved.cropEnabled === "boolean") cropEnabled.checked = saved.cropEnabled;
    if (typeof saved.regionX === "string") regionX.value = saved.regionX;
    if (typeof saved.regionY === "string") regionY.value = saved.regionY;
    if (typeof saved.regionWidth === "string") regionWidth.value = saved.regionWidth;
    if (typeof saved.regionHeight === "string") regionHeight.value = saved.regionHeight;
  } catch {
    /* Ignore corrupted settings. */
  }
}

function selectedSource() {
  return sourcesById.get(source.value) ?? null;
}

function sourceLabel(item: CaptureSource) {
  const size = item.width > 0 && item.height > 0 ? `${item.width}x${item.height}` : "system dialog";
  const kind = item.kind === "test" ? "Test" : item.kind === "portal" ? "Portal" : item.kind;
  return `${kind}: ${item.label} (${size})`;
}

function validationMessage() {
  if (!normalizeEndpoint(endpoint.value)) return "Enter the device address";
  if (token.value.length !== 6) return "Enter the 6-digit pairing code";
  if (!source.value) return "Choose a capture source";
  if (lastDevice && lastDevice.protocolVersion !== 2) return "Flash firmware with low-latency control protocol v2";
  if (lastDevice?.videoTransports?.length && !lastDevice.videoTransports.includes(videoTransport.value)) return `Device does not support ${videoTransport.value.toUpperCase()} video`;
  if (lastDevice?.wifi && lastDevice.wifi !== "connected") return "Device Wi-Fi is not connected";
  if (lastDevice?.activeTransport && lastDevice.activeTransport !== "none") return `Device is busy with ${lastDevice.activeTransport}`;
  return "";
}

/* Address, pairing code, source, and transport only apply at Start; lock them
   while streaming. FPS and quality stay editable and apply live. */
function updateLockedInputs() {
  const locked = streamActive || streamStopping;
  for (const input of [endpoint, token, source, videoTransport, cropEnabled, regionX, regionY, regionWidth, regionHeight]) {
    input.disabled = locked;
  }
  refresh.disabled = locked;
  checkDevice.disabled = locked;
}

function updateReadyState() {
  const message = validationMessage();
  start.disabled = streamActive || streamStopping || Boolean(message);
  stop.disabled = !streamActive || streamStopping;
  readyLabel.textContent = streamStopping ? "Stopping" : streamActive ? "Streaming" : message || "Ready";
  readyLabel.dataset.tone = streamActive || streamStopping ? "active" : message ? "warn" : "good";
  updateLockedInputs();
}

function stopStoppingStatusPoll() {
  if (stoppingStatusPoll !== null) {
    window.clearInterval(stoppingStatusPoll);
    stoppingStatusPoll = null;
  }
}

function ensureStoppingStatusPoll() {
  if (stoppingStatusPoll !== null) return;
  stoppingStatusPoll = window.setInterval(() => {
    invoke<StreamEvent>("get_stream_status").then(applyStreamStatus).catch(() => undefined);
  }, 200);
}

function updateSourceSummary() {
  const item = selectedSource();
  if (!item) {
    sourceSummary.textContent = "No source selected";
    updateReadyState();
    return;
  }
  if (item.id === "test:color-bars") {
    sourceSummary.textContent = "Static color bars test the JPEG and Wi-Fi path without screen capture.";
  } else if (item.id === "linux:portal") {
    sourceSummary.textContent = "The system dialog will ask which screen or window to share.";
  } else {
    sourceSummary.textContent = `Selected ${item.kind} source, ${item.width}x${item.height}.`;
  }
  updateReadyState();
}

function clearPreview() {
  preview.hidden = true;
  preview.removeAttribute("src");
  previewEmpty.hidden = false;
}

function friendlyError(message: string) {
  if (message.includes("device_busy")) return `${message} — press Stop in the other client or restart the device.`;
  if (message.includes("not_negotiated")) return `${message} — restart the stream to renegotiate.`;
  if (message.includes("unsupported_protocol")) return `${message} — flash matching v2 firmware.`;
  return message;
}

function applyDevice(device: DeviceStatus) {
  lastDevice = device;
  const online = device.wifi === "connected" && device.activeTransport === "none";
  setPill(devicePill, online ? "Device ready" : "Device attention", online ? "good" : "warn");
  deviceStatus.textContent = device.ipv4 ? `Online at ${device.ipv4}` : "Online, no IP reported";
  deviceWifi.textContent = device.wifi;
  deviceScreen.textContent = `${device.width}x${device.height}`;
  const udp = device.udp;
  deviceTransport.textContent = device.activeTransport === "udp"
    ? `udp | rx ${udp.rxPackets} | accepted ${udp.acceptedFragments} | rejected ${udp.rejectedPackets} (${udp.lastReject}) | frames ${udp.submittedFrames}`
    : `${device.activeTransport} | device ${Math.round((device.performance.queueWaitUs + device.performance.jpegDecodeUs + device.performance.lcdWaitUs + device.performance.lcdSubmitUs) / 1000)} ms | replaced ${device.performance.replacedFrames}`;
  for (const option of videoTransport.options) option.disabled = !device.videoTransports.includes(option.value);
  if (videoTransport.selectedOptions[0]?.disabled) videoTransport.value = "tcp";
  updateReadyState();
}

function applyMetrics(payload: StreamEvent) {
  metricTransport.textContent = payload.videoTransport === "none" ? "--" : payload.videoTransport.toUpperCase();
  metricFps.textContent = String(payload.fps);
  metricCapture.textContent = `${payload.captureWaitMs} ms`;
  metricEncode.textContent = `${payload.captureEncodeMs} ms`;
  metricDevice.textContent = `${payload.deviceMs} ms`;
  metricReplaced.textContent = String(payload.deviceReplacedFrames);
  metricDropped.textContent = String(payload.droppedFrames);
  metricCapture.classList.toggle("slow", payload.captureWaitMs > SLOW_STAGE_MS);
  metricEncode.classList.toggle("slow", payload.captureEncodeMs > SLOW_STAGE_MS);
  metricDevice.classList.toggle("slow", payload.deviceMs > SLOW_STAGE_MS);
  metricLimited.hidden = payload.captureWaitMs <= SLOW_STAGE_MS
    && payload.captureEncodeMs <= SLOW_STAGE_MS
    && payload.deviceMs <= SLOW_STAGE_MS;
}

function applyStreamStatus(payload: StreamEvent) {
  if (payload.state === "idle") return;
  const state = payload.state.toLowerCase();
  const failed = state === "error";
  const warning = state === "dropping";
  const stopped = state === "stopped";
  const stopping = state === "stopping";
  streamStopping = stopping;
  streamActive = !failed && !stopped;
  if (stopped) {
    stopStoppingStatusPoll();
    lastDevice = null;
    setPill(devicePill, "Device unchecked", "muted");
    deviceTransport.textContent = "--";
    clearPreview();
  } else if (failed) {
    stopStoppingStatusPoll();
    clearPreview();
  }
  setPill(streamPill, stopped ? "Stopped" : payload.state, failed ? "bad" : warning ? "warn" : streamActive ? "active" : "muted");
  setStatus(`${payload.state}: ${friendlyError(payload.detail)}`, failed ? "bad" : warning ? "warn" : stopped ? "normal" : "good");
  applyMetrics(payload);
  updateReadyState();
}

async function probeDevice() {
  const normalized = normalizeEndpoint(endpoint.value);
  endpoint.value = normalized;
  const device = await invoke<DeviceStatus>("probe_wifi_device", { endpoint: normalized });
  applyDevice(device);
  if (device.activeTransport !== "none") throw new Error(`Device is busy with ${device.activeTransport} transport`);
  if (device.wifi !== "connected") throw new Error("Device Wi-Fi is not connected");
  return device;
}

async function checkDeviceNow() {
  setPill(devicePill, "Checking", "active");
  deviceStatus.textContent = "Checking device...";
  try {
    await probeDevice();
    setStatus("Device is reachable", "good");
  } catch (error) {
    lastDevice = null;
    setPill(devicePill, "Device unavailable", "bad");
    deviceStatus.textContent = String(error);
    deviceWifi.textContent = "--";
    deviceScreen.textContent = "--";
    deviceTransport.textContent = "--";
    setStatus(String(error), "bad");
  } finally {
    updateReadyState();
  }
}

/* Re-probe automatically once the user stops editing the address or code. */
function scheduleProbe() {
  if (probeTimer !== null) window.clearTimeout(probeTimer);
  probeTimer = window.setTimeout(() => {
    probeTimer = null;
    if (streamActive || streamStopping) return;
    if (!normalizeEndpoint(endpoint.value) || token.value.length !== 6) return;
    checkDeviceNow().catch(() => undefined);
  }, PROBE_DEBOUNCE_MS);
}

function scheduleParamsUpdate() {
  if (paramsTimer !== null) window.clearTimeout(paramsTimer);
  paramsTimer = window.setTimeout(() => {
    paramsTimer = null;
    if (!streamActive || streamStopping) return;
    const nextFps = Math.min(30, Math.max(1, Number(fps.value) || 25));
    const nextQuality = Math.min(95, Math.max(30, Number(quality.value) || 70));
    invoke("set_stream_params", { fps: nextFps, jpegQuality: nextQuality })
      .catch((error) => setStatus(String(error), "warn"));
  }, PARAMS_DEBOUNCE_MS);
}

async function refreshDeviceDiagnostics() {
  const device = await invoke<DeviceStatus>("probe_wifi_device", { endpoint: normalizeEndpoint(endpoint.value) });
  if (!streamActive && !streamStopping) return;
  applyDevice(device);
}

async function refreshSources() {
  refresh.disabled = true;
  setStatus("Refreshing capture sources...");
  try {
    const sources = await invoke<CaptureSource[]>("list_capture_sources");
    sourcesById.clear();
    for (const item of sources) sourcesById.set(item.id, item);
    source.replaceChildren(...sources.map((item) => {
      const option = document.createElement("option");
      option.value = item.id;
      option.textContent = sourceLabel(item);
      return option;
    }));
    if (!sources.length) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "No capture source available";
      source.replaceChildren(option);
    }
    setStatus(sources.length ? `${sources.length} capture sources found` : "No capture source available", sources.length ? "good" : "warn");
    updateSourceSummary();
  } catch (error) {
    setStatus(String(error), "bad");
  } finally {
    refresh.disabled = false;
    updateReadyState();
  }
}

refresh.onclick = () => refreshSources();

checkDevice.onclick = async () => {
  checkDevice.disabled = true;
  await checkDeviceNow();
};

start.onclick = async () => {
  const message = validationMessage();
  if (message) {
    setStatus(message, "warn");
    updateReadyState();
    return;
  }
  saveSettings();
  start.disabled = true;
  stop.disabled = false;
  streamActive = true;
  streamStopping = false;
  setPill(streamPill, "Starting", "active");
  setStatus("Opening Wi-Fi stream...");
  try {
    /* start_wifi_stream probes the device itself; no separate probe here. */
    await invoke("start_wifi_stream", {
      config: {
        endpoint: normalizeEndpoint(endpoint.value),
        token: token.value,
        sourceId: source.value,
        regionX: cropEnabled.checked ? Number(regionX.value) : 0,
        regionY: cropEnabled.checked ? Number(regionY.value) : 0,
        regionWidth: cropEnabled.checked ? Number(regionWidth.value) : 0,
        regionHeight: cropEnabled.checked ? Number(regionHeight.value) : 0,
        fps: Number(fps.value),
        jpegQuality: Number(quality.value),
        videoTransport: videoTransport.value,
      },
    });
  } catch (error) {
    const message = String(error);
    if (message.includes("still stopping")) {
      streamActive = true;
      streamStopping = true;
      setPill(streamPill, "Stopping", "active");
      ensureStoppingStatusPoll();
    } else if (message.includes("already running")) {
      streamActive = true;
      streamStopping = false;
      stopStoppingStatusPoll();
      setPill(streamPill, "Streaming", "active");
    } else {
      streamActive = false;
      streamStopping = false;
      stopStoppingStatusPoll();
      setPill(streamPill, "Error", "bad");
    }
    setStatus(friendlyError(message), streamActive ? "warn" : "bad");
    updateReadyState();
  }
};

stop.onclick = async () => {
  stop.disabled = true;
  streamActive = true;
  streamStopping = true;
  setPill(streamPill, "Stopping", "active");
  setStatus("Stopping stream...");
  try {
    await invoke("stop_wifi_stream");
    ensureStoppingStatusPoll();
  } catch (error) {
    streamStopping = false;
    stopStoppingStatusPoll();
    setStatus(String(error), "bad");
  } finally {
    updateReadyState();
  }
};

endpoint.addEventListener("blur", () => {
  endpoint.value = normalizeEndpoint(endpoint.value);
  updateReadyState();
});
endpoint.addEventListener("input", () => {
  lastDevice = null;
  setPill(devicePill, "Device unchecked", "muted");
  saveSettings();
  scheduleProbe();
  updateReadyState();
});
token.addEventListener("input", () => {
  sanitizeToken();
  saveSettings();
  scheduleProbe();
  updateReadyState();
});
source.addEventListener("change", updateSourceSummary);
cropEnabled.addEventListener("change", () => {
  cropFields.hidden = !cropEnabled.checked;
  saveSettings();
});
videoTransport.addEventListener("change", saveSettings);
for (const input of [fps, quality]) {
  input.addEventListener("input", () => {
    saveSettings();
    scheduleParamsUpdate();
    updateReadyState();
  });
}
for (const input of [regionX, regionY, regionWidth, regionHeight]) {
  input.addEventListener("input", () => {
    saveSettings();
    updateReadyState();
  });
}

async function initialize() {
  restoreSettings();
  sanitizeToken();
  endpoint.value = normalizeEndpoint(endpoint.value);
  cropFields.hidden = !cropEnabled.checked;
  try {
    await listen<StreamEvent>("stream-status", ({ payload }) => applyStreamStatus(payload));
    await listen<string>("stream-preview", ({ payload }) => {
      preview.src = `data:image/jpeg;base64,${payload}`;
      preview.hidden = false;
      previewEmpty.hidden = true;
    });
  } catch (error) {
    /* Keep polling and source discovery alive even if the event channel
       is rejected (e.g. missing capability ACL). */
    setStatus(`Event channel unavailable: ${String(error)}`, "bad");
  }
  await refreshSources();
  window.setInterval(() => {
    invoke<StreamEvent>("get_stream_status").then(applyStreamStatus).catch(() => undefined);
    /* TCP already reports device timings per frame; only UDP needs the
       HTTP diagnostics, and 2 s keeps the device HTTP server quiet. */
    const now = Date.now();
    if (streamActive && videoTransport.value === "udp" && now - lastDevicePoll >= DEVICE_POLL_MS) {
      lastDevicePoll = now;
      refreshDeviceDiagnostics().catch(() => undefined);
    }
  }, 300);
  if (normalizeEndpoint(endpoint.value) && token.value.length === 6) {
    checkDeviceNow().catch(() => undefined);
  }
  updateReadyState();
}

initialize().catch((error) => setStatus(String(error), "bad"));
