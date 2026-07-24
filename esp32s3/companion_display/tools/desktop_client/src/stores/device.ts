import { defineStore } from "pinia";
import { computed, ref } from "vue";
import type { DeviceStatus } from "../types/tauri";

export const useDeviceStore = defineStore("device", () => {
  const info = ref<DeviceStatus | null>(null);
  const checking = ref(false);
  const devicePillText = ref("Device unchecked");
  const devicePillTone = ref<"muted" | "good" | "warn" | "bad" | "active">("muted");
  const statusLine = ref("Not checked");
  const wifi = ref("--");
  const screen = ref("--");
  const transport = ref("--");
  const connection = ref("Not checked");

  const online = computed(() => info.value?.wifi === "connected" && info.value?.activeTransport === "none");
  const protocolOk = computed(() => info.value?.protocolVersion === 2);
  const supportedTransports = computed(() => info.value?.videoTransports ?? []);
  const ackTotalMs = computed(() => {
    const p = info.value?.performance;
    if (!p) return 0;
    return Math.round((p.queueWaitUs + p.jpegDecodeUs + p.lcdWaitUs + p.lcdSubmitUs) / 1000);
  });

  function apply(device: DeviceStatus) {
    info.value = device;
    const ready = device.wifi === "connected" && device.activeTransport === "none";
    devicePillText.value = ready ? "Device ready" : "Device attention";
    devicePillTone.value = ready ? "good" : "warn";
    connection.value = device.ipv4 ? `Online at ${device.ipv4}` : "Online, no IP reported";
    wifi.value = device.wifi;
    screen.value = `${device.width}x${device.height}`;
    const udp = device.udp;
    transport.value =
      device.activeTransport === "udp"
        ? `udp | rx ${udp.rxPackets} | accepted ${udp.acceptedFragments} | rejected ${udp.rejectedPackets} (${udp.lastReject}) | frames ${udp.submittedFrames}`
        : `${device.activeTransport} | device ${ackTotalMs.value} ms | replaced ${device.performance.replacedFrames}`;
  }

  function reset() {
    info.value = null;
    devicePillText.value = "Device unchecked";
    devicePillTone.value = "muted";
    statusLine.value = "Not checked";
    wifi.value = "--";
    screen.value = "--";
    transport.value = "--";
    connection.value = "Not checked";
  }

  function setChecking(text: string = "Checking device...") {
    devicePillText.value = "Checking";
    devicePillTone.value = "active";
    statusLine.value = text;
    connection.value = text;
  }

  function setUnavailable(message: string) {
    info.value = null;
    devicePillText.value = "Device unavailable";
    devicePillTone.value = "bad";
    statusLine.value = message;
    connection.value = message;
    wifi.value = "--";
    screen.value = "--";
    transport.value = "--";
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