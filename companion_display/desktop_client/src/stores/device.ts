import { defineStore } from "pinia";
import { computed, ref } from "vue";
import type { DeviceStatus } from "../types/tauri";
import { useLocaleStore } from "./locale";

type DeviceViewState = "unchecked" | "checking" | "available" | "unavailable";

export const useDeviceStore = defineStore("device", () => {
  const locale = useLocaleStore();
  const info = ref<DeviceStatus | null>(null);
  const checking = ref(false);
  const viewState = ref<DeviceViewState>("unchecked");
  const rawError = ref("");

  const online = computed(() => info.value?.wifi === "connected" && info.value?.activeTransport === "none");
  const protocolOk = computed(() => info.value?.protocolVersion === 3);
  const supportedTransports = computed(() => info.value?.videoTransports ?? []);
  const ackTotalMs = computed(() => {
    const p = info.value?.performance;
    if (!p) return 0;
    return Math.round((p.queueWaitUs + p.jpegDecodeUs + p.lcdWaitUs + p.lcdSubmitUs) / 1000);
  });

  const devicePillText = computed(() => {
    if (viewState.value === "checking") return locale.t("device.checking");
    if (viewState.value === "unavailable") return locale.t("device.unavailable");
    if (viewState.value === "available") return online.value ? locale.t("device.ready") : locale.t("device.attention");
    return locale.t("device.unchecked");
  });
  const devicePillTone = computed<"muted" | "good" | "warn" | "bad" | "active">(() => {
    if (viewState.value === "checking") return "active";
    if (viewState.value === "unavailable") return "bad";
    if (viewState.value === "available") return online.value ? "good" : "warn";
    return "muted";
  });
  const statusLine = computed(() => {
    if (viewState.value === "checking") return locale.t("device.checkingDetail");
    if (viewState.value === "unavailable") return rawError.value;
    return connection.value;
  });
  const connection = computed(() => {
    if (viewState.value === "checking") return locale.t("device.checkingDetail");
    if (viewState.value === "unavailable") return rawError.value;
    if (!info.value) return locale.t("device.notChecked");
    return info.value.ipv4
      ? locale.t("device.onlineAt", { address: info.value.ipv4 })
      : locale.t("device.onlineNoIp");
  });
  const wifi = computed(() => {
    const value = info.value?.wifi;
    if (!value) return "--";
    const keys = {
      connected: "device.wifi.connected",
      connecting: "device.wifi.connecting",
      setup_required: "device.wifi.setup_required",
    } as const;
    return value in keys ? locale.t(keys[value as keyof typeof keys]) : value;
  });
  const screen = computed(() => (info.value ? `${info.value.width}x${info.value.height}` : "--"));
  const transport = computed(() => {
    const device = info.value;
    if (!device) return "--";
    if (device.activeTransport === "udp") {
      const udp = device.udp;
      return locale.t("device.transport.udpSummary", {
        rx: udp.rxPackets,
        accepted: udp.acceptedFragments,
        rejected: udp.rejectedPackets,
        reason: udp.lastReject,
        frames: udp.submittedFrames,
      });
    }
    const active = device.activeTransport === "none" ? locale.t("device.transport.none") : device.activeTransport.toUpperCase();
    return locale.t("device.transport.summary", {
      transport: active,
      deviceMs: ackTotalMs.value,
      replaced: device.performance.replacedFrames,
    });
  });
  const touch = computed(() => {
    const device = info.value;
    if (!device) return "--";
    if (!device.touch) return locale.t("device.touch.unsupported");
    return device.touchEventsActive
      ? locale.t("device.touch.active")
      : locale.t("device.touch.supported");
  });

  function apply(device: DeviceStatus) {
    info.value = device;
    viewState.value = "available";
    rawError.value = "";
  }

  function reset() {
    info.value = null;
    viewState.value = "unchecked";
    rawError.value = "";
  }

  function setChecking() {
    viewState.value = "checking";
    rawError.value = "";
  }

  function setUnavailable(message: string) {
    info.value = null;
    viewState.value = "unavailable";
    rawError.value = message;
  }

  return {
    info,
    checking,
    devicePillText,
    devicePillTone,
    statusLine,
    wifi,
    screen,
    transport,
    touch,
    connection,
    online,
    protocolOk,
    supportedTransports,
    ackTotalMs,
    apply,
    reset,
    setChecking,
    setUnavailable,
  };
});
