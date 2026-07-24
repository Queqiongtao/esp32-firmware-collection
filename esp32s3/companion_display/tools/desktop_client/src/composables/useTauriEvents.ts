import { listen } from "@tauri-apps/api/event";
import { useDeviceStore } from "../stores/device";
import { useLogsStore } from "../stores/logs";
import { useStreamStore } from "../stores/stream";
import { useSettingsStore } from "../stores/settings";
import { usePreviewSink } from "../composables/usePreviewSink";

export function useTauriEvents() {
  const device = useDeviceStore();
  const stream = useStreamStore();
  const logs = useLogsStore();
  const settings = useSettingsStore();
  const preview = usePreviewSink();

  async function init() {
    try {
      await listen<import("../types/tauri").StreamEvent>("stream-status", ({ payload }) => {
        const wasStopped = stream.state === "stopped";
        stream.applyStreamStatus(payload);
        const tone =
          stream.headlineTone === "bad" ? "bad" : stream.headlineTone === "warn" ? "warn" : stream.headlineTone === "good" ? "good" : "normal";
        logs.push(stream.headlineText, tone);
        if (payload.state.toLowerCase() === "stopped" && !wasStopped) {
          device.reset();
          preview.clear();
          stream.clearMetrics();
        } else if (payload.state.toLowerCase() === "error" && !wasStopped) {
          preview.clear();
          stream.clearMetrics();
        }
      });
      /* 预览渲染入口：高帧化时改 usePreviewSink 内部实现（Blob URL / canvas）。 */
      await listen<string>("stream-preview", ({ payload }) => {
        preview.display(payload);
      });
    } catch (error) {
      logs.push(`Event channel unavailable: ${String(error)}`, "bad");
    }
  }

  return { init };
}