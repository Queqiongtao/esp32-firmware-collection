import { invoke } from "@tauri-apps/api/core";
import { computed, onUnmounted, ref } from "vue";
import { useDeviceStore } from "../stores/device";
import { useLogsStore } from "../stores/logs";
import { useStreamStore } from "../stores/stream";
import { useSettingsStore } from "../stores/settings";
import type { CaptureSource, DeviceStatus, StreamEvent } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";

const PROBE_DEBOUNCE_MS = 800;
const PARAMS_DEBOUNCE_MS = 300;
const DEVICE_POLL_MS = 2000;
const STATUS_POLL_MS = 300;

function clamp(value: number, min: number, max: number, fallback: number): number {
  const parsed = Number.isFinite(value) ? value : fallback;
  return Math.min(max, Math.max(min, parsed));
}

export function useStreamLifecycle() {
  const device = useDeviceStore();
  const stream = useStreamStore();
  const logs = useLogsStore();
  const settings = useSettingsStore();
  const locale = useLocaleStore();

  function friendlyError(message: string): string {
    if (message.includes("device_busy")) return `${message} — ${locale.t("error.deviceBusyHelp")}`;
    if (message.includes("not_negotiated")) return `${message} — ${locale.t("error.notNegotiatedHelp")}`;
    if (message.includes("unsupported_protocol")) return `${message} — ${locale.t("error.protocolHelp")}`;
    return message;
  }

  const sources = ref<CaptureSource[]>([]);

  const validationMessage = computed(() => {
    if (!settings.normalizedEndpoint) return locale.t("validation.address");
    if (settings.token.length !== 6) return locale.t("validation.token");
    if (!settings.sourceId) return locale.t("validation.source");
    if (device.info && device.info.protocolVersion !== 2) return locale.t("validation.protocol");
    const supported = device.supportedTransports;
    if (supported.length && !supported.includes(settings.videoTransport)) {
      return locale.t("validation.transport", { transport: settings.videoTransport.toUpperCase() });
    }
    if (device.info?.wifi && device.info.wifi !== "connected") return locale.t("validation.wifi");
    if (device.info?.activeTransport && device.info.activeTransport !== "none") {
      return locale.t("validation.busy", { transport: device.info.activeTransport.toUpperCase() });
    }
    return "";
  });

  const locked = computed(() => stream.active || stream.stopping);

  const startDisabled = computed(() => stream.active || stream.stopping || Boolean(validationMessage.value));
  const stopDisabled = computed(() => !stream.active || stream.stopping);

  const readyLabel = computed(() => {
    if (stream.stopping) return locale.t("status.stopping");
    if (stream.active) return locale.t("status.streaming");
    const message = validationMessage.value;
    return message || locale.t("status.ready");
  });
  const readyTone = computed<"good" | "warn" | "active">(() =>
    stream.active || stream.stopping ? "active" : validationMessage.value ? "warn" : "good",
  );

  async function probeDevice(): Promise<DeviceStatus> {
    const endpoint = settings.normalizedEndpoint;
    settings.normalizeEndpointInPlace();
    const dev = await invoke<DeviceStatus>("probe_wifi_device", { endpoint });
    device.apply(dev);
    if (dev.activeTransport !== "none") throw new Error(locale.t("validation.busy", { transport: dev.activeTransport.toUpperCase() }));
    if (dev.wifi !== "connected") throw new Error(locale.t("validation.wifi"));
    return dev;
  }

  async function checkDeviceNow() {
    device.checking = true;
    device.setChecking();
    logs.pushKey("log.checkingDevice", "normal");
    try {
      await probeDevice();
      logs.pushKey("log.deviceReachable", "good");
    } catch (error) {
      device.setUnavailable(String(error));
      logs.pushRaw(String(error), "bad");
    } finally {
      device.checking = false;
    }
  }

  async function refreshSources() {
    stream.refreshBusy = true;
    logs.pushKey("log.refreshingSources", "normal");
    try {
      const list = await invoke<CaptureSource[]>("list_capture_sources");
      sources.value = list;
      if (!list.length) logs.pushKey("log.noSources", "warn");
      else logs.pushKey("log.sourcesFound", "good", { count: list.length });
      if (settings.sourceId && !list.some((s) => s.id === settings.sourceId)) settings.sourceId = "";
      stream.refreshSourcesDone++;
    } catch (error) {
      logs.pushRaw(String(error), "bad");
    } finally {
      stream.refreshBusy = false;
    }
  }

  let probeTimer: number | null = null;
  function scheduleProbe() {
    if (probeTimer !== null) window.clearTimeout(probeTimer);
    probeTimer = window.setTimeout(() => {
      probeTimer = null;
      if (stream.active || stream.stopping) return;
      if (!settings.normalizedEndpoint || settings.token.length !== 6) return;
      checkDeviceNow().catch(() => undefined);
    }, PROBE_DEBOUNCE_MS);
  }

  let paramsTimer: number | null = null;
  function scheduleParamsUpdate() {
    if (paramsTimer !== null) window.clearTimeout(paramsTimer);
    paramsTimer = window.setTimeout(() => {
      paramsTimer = null;
      if (!stream.active || stream.stopping) return;
      const nextFps = clamp(settings.fps, 1, 30, 25);
      const nextQuality = clamp(settings.quality, 30, 95, 70);
      invoke("set_stream_params", { fps: nextFps, jpegQuality: nextQuality }).catch((error) =>
        logs.pushRaw(String(error), "warn"),
      );
    }, PARAMS_DEBOUNCE_MS);
  }

  async function start() {
    const message = validationMessage.value;
    if (message) {
      logs.pushRaw(message, "warn");
      return;
    }
    settings.save();
    stream.starting();
    logs.pushKey("log.openingStream", "normal");
    try {
      await invoke("start_wifi_stream", {
        config: {
          endpoint: settings.normalizedEndpoint,
          token: settings.token,
          sourceId: settings.sourceId,
          regionX: settings.cropEnabled ? settings.regionX : 0,
          regionY: settings.cropEnabled ? settings.regionY : 0,
          regionWidth: settings.cropEnabled ? settings.regionWidth : 0,
          regionHeight: settings.cropEnabled ? settings.regionHeight : 0,
          fps: settings.fps,
          jpegQuality: settings.quality,
          videoTransport: settings.videoTransport,
          displayOrientation: settings.displayOrientation,
        },
      });
    } catch (error) {
      const msg = String(error);
      if (msg.includes("still stopping")) {
        stream.active = true;
        stream.stopping = true;
        stream.stopBegin();
      } else if (msg.includes("already running")) {
        stream.setStreaming();
      } else {
        stream.setStreamError();
      }
      logs.pushRaw(friendlyError(msg), stream.active ? "warn" : "bad");
    }
  }

  async function stop() {
    stream.stopBegin();
    logs.pushKey("log.stoppingStream", "normal");
    try {
      await invoke("stop_wifi_stream");
    } catch (error) {
      stream.stopping = false;
      logs.pushRaw(String(error), "bad");
    }
  }

  async function refreshDeviceDiagnostics() {
    const dev = await invoke<DeviceStatus>("probe_wifi_device", { endpoint: settings.normalizedEndpoint });
    if (!stream.active && !stream.stopping) return;
    device.apply(dev);
  }

  let statusTimer: number | null = null;
  let lastDevicePoll = 0;
  function startPolling() {
    if (statusTimer !== null) return;
    statusTimer = window.setInterval(() => {
      invoke<StreamEvent>("get_stream_status")
        .then((payload) => stream.applyStreamStatus(payload))
        .catch(() => undefined);
      const now = Date.now();
      if (stream.active && settings.videoTransport === "udp" && now - lastDevicePoll >= DEVICE_POLL_MS) {
        lastDevicePoll = now;
        refreshDeviceDiagnostics().catch(() => undefined);
      }
    }, STATUS_POLL_MS);
  }

  function sourceLabel(item: CaptureSource): string {
    const size = item.width > 0 && item.height > 0 ? `${item.width}x${item.height}` : locale.t("source.systemDialog");
    const kindKeys = {
      test: "source.kind.test",
      portal: "source.kind.portal",
      monitor: "source.kind.monitor",
      window: "source.kind.window",
    } as const;
    const kind = item.kind in kindKeys ? locale.t(kindKeys[item.kind as keyof typeof kindKeys]) : item.kind;
    const label = item.id === "test:color-bars" ? locale.t("source.colorBars") : item.id === "linux:portal" ? locale.t("source.portal") : item.label;
    return `${kind}: ${label} (${size})`;
  }

  onUnmounted(() => {
    if (statusTimer !== null) window.clearInterval(statusTimer);
    if (probeTimer !== null) window.clearTimeout(probeTimer);
    if (paramsTimer !== null) window.clearTimeout(paramsTimer);
  });

  return {
    sources,
    sourceLabel,
    validationMessage,
    locked,
    startDisabled,
    stopDisabled,
    readyLabel,
    readyTone,
    checkDeviceNow,
    refreshSources,
    scheduleProbe,
    scheduleParamsUpdate,
    start,
    stop,
    startPolling,
  };
}
