import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import "./style.css";

type CaptureSource = { id: string; kind: string; label: string; width: number; height: number };
type StreamEvent = { state: string; detail: string; fps: number; jpegBytes: number; droppedFrames: number };
type DeviceStatus = { name: string; width: number; height: number; wifi: string; ipv4: string; activeTransport: string; tokenSet: boolean };

const byId = <T extends HTMLElement>(id: string) => document.querySelector<T>(`#${id}`)!;
const source = byId<HTMLSelectElement>("source");
const status = byId<HTMLParagraphElement>("status");
const metrics = byId<HTMLParagraphElement>("metrics");
const start = byId<HTMLButtonElement>("start");
const stop = byId<HTMLButtonElement>("stop");
const checkDevice = byId<HTMLButtonElement>("check-device");
const deviceStatus = byId<HTMLParagraphElement>("device-status");

function applyStreamStatus(payload: StreamEvent) {
  if (payload.state === "idle") return;
  status.textContent = `${payload.state}: ${payload.detail}`;
  metrics.textContent = `FPS ${payload.fps} | JPEG ${payload.jpegBytes} bytes | Dropped ${payload.droppedFrames}`;
  if (payload.state === "stopped" || payload.state === "error") {
    start.disabled = !source.value;
    stop.disabled = true;
  }
}

async function probeDevice() {
  const endpoint = byId<HTMLInputElement>("endpoint").value.trim();
  const device = await invoke<DeviceStatus>("probe_wifi_device", { endpoint });
  deviceStatus.textContent = `Online | Wi-Fi ${device.wifi} | ${device.ipv4 || "no IP"} | ${device.width}x${device.height} | ${device.activeTransport}`;
  if (device.activeTransport !== "none") throw new Error(`Device is busy with ${device.activeTransport} transport`);
  if (device.wifi !== "connected") throw new Error("Device Wi-Fi is not connected");
  return device;
}

async function refreshSources() {
  const sources = await invoke<CaptureSource[]>("list_capture_sources");
  source.replaceChildren(...sources.map((item) => {
    const option = document.createElement("option");
    option.value = item.id;
    option.textContent = `${item.kind}: ${item.label} (${item.width}x${item.height})`;
    return option;
  }));
  status.textContent = sources.length ? `${sources.length} capture sources found` : "No capture source available";
  if (stop.disabled) start.disabled = sources.length === 0;
}

byId<HTMLButtonElement>("refresh").onclick = () => refreshSources().catch((error) => status.textContent = String(error));
checkDevice.onclick = async () => {
  checkDevice.disabled = true;
  deviceStatus.textContent = "Checking device...";
  try {
    await probeDevice();
  } catch (error) {
    deviceStatus.textContent = `Unavailable: ${String(error)}`;
  } finally {
    checkDevice.disabled = false;
  }
};
start.onclick = async () => {
  if (!source.value) {
    status.textContent = "Select a capture source before starting";
    return;
  }
  start.disabled = true;
  stop.disabled = false;
  status.textContent = "connecting: Opening Wi-Fi connection";
  metrics.textContent = "";
  try {
    await probeDevice();
    await invoke("start_wifi_stream", {
      config: {
        endpoint: byId<HTMLInputElement>("endpoint").value.trim(), token: byId<HTMLInputElement>("token").value.trim(),
        sourceId: source.value, regionX: Number(byId<HTMLInputElement>("region-x").value), regionY: Number(byId<HTMLInputElement>("region-y").value),
        regionWidth: Number(byId<HTMLInputElement>("region-width").value), regionHeight: Number(byId<HTMLInputElement>("region-height").value),
        fps: Number(byId<HTMLInputElement>("fps").value), jpegQuality: Number(byId<HTMLInputElement>("quality").value),
      },
    });
  } catch (error) {
    status.textContent = String(error);
    start.disabled = false;
    stop.disabled = true;
  }
};
stop.onclick = async () => { await invoke("stop_wifi_stream"); start.disabled = false; stop.disabled = true; };

async function initialize() {
  await listen<StreamEvent>("stream-status", ({ payload }) => applyStreamStatus(payload));
  await refreshSources();
  window.setInterval(() => {
    invoke<StreamEvent>("get_stream_status").then(applyStreamStatus).catch(() => undefined);
  }, 300);
}

initialize().catch((error) => status.textContent = String(error));
