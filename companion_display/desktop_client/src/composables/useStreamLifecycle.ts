import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { computed, onUnmounted, ref } from "vue";
import { useDeviceStore } from "../stores/device";
import { useLogsStore } from "../stores/logs";
import { useStreamStore } from "../stores/stream";
import { useSettingsStore } from "../stores/settings";
import type { CaptureRegion, CaptureSource, DeviceStatus, RegionSelectionPreview, StreamEvent, StreamTarget } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";

const PROBE_DEBOUNCE_MS = 800;
const PARAMS_DEBOUNCE_MS = 300;
const DEVICE_POLL_MS = 2000;
const STATUS_POLL_MS = 300;

type RegionSelectionResult =
  | { action: "confirm"; region: CaptureRegion }
  | { action: "cancel" | "change-source" };

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
    if (settings.projectionMode === "region" && settings.sourceId === "test:color-bars") return locale.t("validation.regionSource");
    if (device.info && device.info.protocolVersion !== 3) return locale.t("validation.protocol");
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

  const locked = computed(() => stream.regionBusy || stream.active || stream.stopping);

  const startDisabled = computed(() => {
    if (stream.active && settings.projectionMode === "region") return stream.regionBusy || stream.stopping;
    return stream.regionBusy || stream.stopping || stream.active || Boolean(validationMessage.value);
  });
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
      if (!settings.sourceId && list.some((source) => source.id === "linux:portal")) {
        settings.sourceId = "linux:portal";
        logs.pushKey("log.portalDefaulted", "good");
      }
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

  const regionPreview = ref<RegionSelectionPreview | null>(null);
  const regionCanChangeSource = ref(false);
  let regionResolver: ((result: RegionSelectionResult) => void) | null = null;

  async function chooseRegion(preview: RegionSelectionPreview, canChangeSource = false): Promise<RegionSelectionResult> {
    const window = getCurrentWindow();
    const wasFullscreen = await window.isFullscreen();
    await window.setFullscreen(true);
    await new Promise<void>((resolve) => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
    regionCanChangeSource.value = canChangeSource;
    regionPreview.value = preview;
    const result = await new Promise<RegionSelectionResult>((resolve) => { regionResolver = resolve; });
    regionResolver = null;
    regionPreview.value = null;
    regionCanChangeSource.value = false;
    if (!wasFullscreen) await window.setFullscreen(false);
    return result;
  }

  function confirmRegion(region: CaptureRegion) { regionResolver?.({ action: "confirm", region }); }
  function cancelRegion() { regionResolver?.({ action: "cancel" }); }
  function changeRegionSource() { regionResolver?.({ action: "change-source" }); }

  function streamConfig(region?: CaptureRegion, selectionId?: string) {
    return {
      endpoint: settings.normalizedEndpoint,
      token: settings.token,
      sourceId: settings.sourceId,
      regionX: region?.x ?? 0,
      regionY: region?.y ?? 0,
      regionWidth: region?.width ?? 0,
      regionHeight: region?.height ?? 0,
      fps: settings.fps,
      jpegQuality: settings.quality,
      videoTransport: settings.videoTransport,
      displayOrientation: settings.displayOrientation,
      portalRestoreToken: settings.portalRestoreToken || undefined,
      projectionMode: settings.projectionMode,
      selectionId,
      touchControl: settings.touchControl,
    };
  }

  async function start() {
    const message = validationMessage.value;
    if (message) {
      logs.pushRaw(message, "warn");
      return;
    }
    settings.save();
    if (settings.projectionMode === "region" && stream.active) {
      stream.regionBusy = true;
      logs.pushKey("log.selectingRegion", "normal");
      try {
        const preview = await invoke<RegionSelectionPreview>("prepare_region_reselection");
        const result = await chooseRegion(preview);
        if (result.action === "confirm") {
          await invoke("replace_stream_region", { region: result.region });
          logs.pushKey("log.regionReplaced", "good");
        }
      } catch (error) {
        logs.pushRaw(String(error), "warn");
      } finally {
        stream.regionBusy = false;
      }
      return;
    }
    stream.regionBusy = settings.projectionMode === "region";
    let streamMarkedStarted = false;
    let preparedSelectionId: string | undefined;
    try {
      const dev = await probeDevice();
      let region: CaptureRegion | undefined;
      if (settings.projectionMode === "region") {
        logs.pushKey("log.selectingRegion", "normal");
        let forcePortalChooser = false;
        while (!region) {
          const usedRememberedPortal = settings.sourceId === "linux:portal"
            && Boolean(settings.portalRestoreToken)
            && !forcePortalChooser;
          const config = streamConfig();
          if (forcePortalChooser) config.portalRestoreToken = undefined;
          const preview = await invoke<RegionSelectionPreview>("prepare_region_selection", {
            config, targetWidth: dev.width, targetHeight: dev.height,
          });
          preparedSelectionId = preview.id;
          const result = await chooseRegion(preview, usedRememberedPortal);
          if (result.action === "confirm") {
            region = result.region;
            break;
          }
          await invoke("cancel_region_selection", { selectionId: preview.id });
          preparedSelectionId = undefined;
          if (result.action === "cancel") return;
          settings.clearPortalRestoreToken();
          settings.save();
          forcePortalChooser = true;
          logs.pushKey("log.portalReselecting", "normal");
        }
      }
      stream.setTarget(null);
      stream.starting();
      streamMarkedStarted = true;
      logs.pushKey("log.openingStream", "normal");
      const target = await invoke<StreamTarget>("start_wifi_stream", {
        config: {
          ...streamConfig(region, preparedSelectionId),
        },
      });
      if (stream.active) stream.setTarget(target);
    } catch (error) {
      if (preparedSelectionId) {
        invoke("cancel_region_selection", { selectionId: preparedSelectionId }).catch(() => undefined);
      }
      const msg = String(error);
      if (msg.includes("still stopping")) {
        stream.active = true;
        stream.stopping = true;
        stream.stopBegin();
      } else if (msg.includes("already running")) {
        stream.setStreaming();
      } else if (streamMarkedStarted) {
        stream.setStreamError();
      }
      logs.pushRaw(friendlyError(msg), stream.active ? "warn" : "bad");
    } finally {
      stream.regionBusy = false;
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
    const size = item.id === "test:color-bars"
      ? locale.t("source.followsDevice")
      : item.width > 0 && item.height > 0
        ? `${item.width}x${item.height}`
        : locale.t("source.systemDialog");
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
    regionPreview,
    regionCanChangeSource,
    confirmRegion,
    cancelRegion,
    changeRegionSource,
  };
}
