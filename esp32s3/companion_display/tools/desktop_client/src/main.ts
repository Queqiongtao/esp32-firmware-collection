import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import "./style.css";

type CaptureSource = { id: string; kind: string; label: string; width: number; height: number };
type StreamEvent = { state: string; detail: string; fps: number; jpegBytes: number; droppedFrames: number };
type DeviceStatus = { name: string; width: number; height: number; wifi: string; ipv4: string; activeTransport: string; tokenSet: boolean };

const byId = <T extends HTMLElement>(id: string) => document.querySelector<T>(`#${id}`)!;

const endpoint = byId<HTMLInputElement>("endpoint");
const token = byId<HTMLInputElement>("token");
const source = byId<HTMLSelectElement>("source");
const sourceSummary = byId<HTMLDivElement>("source-summary");
const status = byId<HTMLParagraphElement>("status");
const metrics = byId<HTMLParagraphElement>("metrics");
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

const sourcesById = new Map<string, CaptureSource>();
let lastDevice: DeviceStatus | null = null;
let streamActive = false;

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

function selectedSource() {
  return sourcesById.get(source.value) ?? null;
}

function sourceLabel(item: CaptureSource) {
  const size = item.width > 0 && item.height > 0 ? `${item.width}x${item.height}` : "system dialog";
  const kind = item.kind === "test" ? "Test" : item.kind === "wayland" ? "Wayland" : item.kind;
  return `${kind}: ${item.label} (${size})`;
}

function validationMessage() {
  if (!normalizeEndpoint(endpoint.value)) return "Enter the device address";
  if (token.value.length !== 6) return "Enter the 6-digit pairing code";
  if (!source.value) return "Choose a capture source";
  if (lastDevice?.wifi && lastDevice.wifi !== "connected") return "Device Wi-Fi is not connected";
  if (lastDevice?.activeTransport && lastDevice.activeTransport !== "none") return `Device is busy with ${lastDevice.activeTransport}`;
  return "";
}

function updateReadyState() {
  const message = validationMessage();
  start.disabled = streamActive || Boolean(message);
  stop.disabled = !streamActive;
  readyLabel.textContent = streamActive ? "Streaming" : message || "Ready";
  readyLabel.dataset.tone = streamActive ? "active" : message ? "warn" : "good";
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
  } else if (item.id === "wayland:portal") {
    sourceSummary.textContent = "The system dialog will ask which screen or window to share.";
  } else {
    sourceSummary.textContent = `Selected ${item.kind} source, ${item.width}x${item.height}.`;
  }
  updateReadyState();
}

function applyDevice(device: DeviceStatus) {
  lastDevice = device;
  const online = device.wifi === "connected" && device.activeTransport === "none";
  setPill(devicePill, online ? "Device ready" : "Device attention", online ? "good" : "warn");
  deviceStatus.textContent = device.ipv4 ? `Online at ${device.ipv4}` : "Online, no IP reported";
  deviceWifi.textContent = device.wifi;
  deviceScreen.textContent = `${device.width}x${device.height}`;
  deviceTransport.textContent = device.activeTransport;
  updateReadyState();
}

function applyStreamStatus(payload: StreamEvent) {
  if (payload.state === "idle") return;
  const state = payload.state.toLowerCase();
  const failed = state === "error" || state === "dropping";
  const stopped = state === "stopped";
  streamActive = !failed && !stopped;
  setPill(streamPill, stopped ? "Stopped" : payload.state, failed ? "bad" : streamActive ? "active" : "muted");
  setStatus(`${payload.state}: ${payload.detail}`, failed ? "bad" : stopped ? "normal" : "good");
  metrics.textContent = `FPS ${payload.fps} | JPEG ${payload.jpegBytes} bytes | Dropped ${payload.droppedFrames}`;
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
    checkDevice.disabled = false;
    updateReadyState();
  }
};

start.onclick = async () => {
  const message = validationMessage();
  if (message) {
    setStatus(message, "warn");
    updateReadyState();
    return;
  }
  start.disabled = true;
  stop.disabled = false;
  streamActive = true;
  setPill(streamPill, "Starting", "active");
  setStatus("Checking device before streaming...");
  metrics.textContent = "FPS 0 | JPEG 0 bytes | Dropped 0";
  try {
    await probeDevice();
    setStatus("Opening Wi-Fi stream...");
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
      },
    });
  } catch (error) {
    streamActive = false;
    setPill(streamPill, "Error", "bad");
    setStatus(String(error), "bad");
    updateReadyState();
  }
};

stop.onclick = async () => {
  stop.disabled = true;
  setStatus("Stopping stream...");
  await invoke("stop_wifi_stream");
  streamActive = false;
  setPill(streamPill, "Stopped", "muted");
  updateReadyState();
};

endpoint.addEventListener("blur", () => {
  endpoint.value = normalizeEndpoint(endpoint.value);
  updateReadyState();
});
endpoint.addEventListener("input", () => {
  lastDevice = null;
  setPill(devicePill, "Device unchecked", "muted");
  updateReadyState();
});
token.addEventListener("input", () => {
  sanitizeToken();
  updateReadyState();
});
source.addEventListener("change", updateSourceSummary);
cropEnabled.addEventListener("change", () => {
  cropFields.hidden = !cropEnabled.checked;
});
for (const input of [fps, quality, regionX, regionY, regionWidth, regionHeight]) {
  input.addEventListener("input", updateReadyState);
}

async function initialize() {
  sanitizeToken();
  endpoint.value = normalizeEndpoint(endpoint.value);
  cropFields.hidden = !cropEnabled.checked;
  await listen<StreamEvent>("stream-status", ({ payload }) => applyStreamStatus(payload));
  await refreshSources();
  window.setInterval(() => {
    invoke<StreamEvent>("get_stream_status").then(applyStreamStatus).catch(() => undefined);
  }, 300);
  updateReadyState();
}

initialize().catch((error) => setStatus(String(error), "bad"));
