import { defineStore } from "pinia";
import { computed, ref } from "vue";
import type { ProjectionMode, StreamEvent, StreamTarget } from "../types/tauri";
import { useSettingsStore } from "./settings";
import { streamStateKey, useLocaleStore } from "./locale";

/* 性能受限判定参数：窗口帧数 + 帧预算安全裕度。阈值随用户 FPS 自适应，
   并以最近 STAGE_WINDOW 帧的最不利值平滑，避免单帧抖动闪烁。 */
const STAGE_WINDOW = 5;
const STAGE_MARGIN = 0.9;

function pushWindow(window: number[], value: number): number[] {
  window.push(value);
  if (window.length > STAGE_WINDOW) window.splice(0, window.length - STAGE_WINDOW);
  return window;
}

export const useStreamStore = defineStore("stream", () => {
  const settings = useSettingsStore();
  const locale = useLocaleStore();

  const active = ref(false);
  const stopping = ref(false);
  const regionBusy = ref(false);
  const activeMode = ref<ProjectionMode>("live");
  const state = ref("idle");
  const detail = ref("");
  const pillMode = ref<"idle" | "starting" | "stopping" | "streaming" | "error" | "event">("idle");
  const pillTone = ref<"muted" | "good" | "warn" | "bad" | "active">("muted");

  const pillText = computed(() => {
    const fixed = {
      idle: "status.idle",
      starting: "status.starting",
      stopping: "status.stopping",
      streaming: "status.streaming",
      error: "status.error",
    } as const;
    if (pillMode.value !== "event") return locale.t(fixed[pillMode.value]);
    const key = streamStateKey(state.value);
    return key ? locale.t(key) : state.value;
  });

  const metrics = ref({
    videoTransport: "none",
    fps: 0,
    jpegBytes: 0,
    jpegQualityUsed: 0,
    sourceScalePercent: 0,
    captureWaitMs: 0,
    captureProcessMs: 0,
    captureEncodeMs: 0,
    sendMs: 0,
    jpegEncodeAttempts: 0,
    deviceMs: 0,
    deviceReplacedFrames: 0,
    droppedFrames: 0,
  });

  const captureWindow = ref<number[]>([]);
  const encodeWindow = ref<number[]>([]);
  const sendWindow = ref<number[]>([]);
  const deviceWindow = ref<number[]>([]);

  const previewSrc = ref<string>("");
  const previewReady = ref(false);
  const target = ref<StreamTarget | null>(null);

  const streamStarting = ref(false);
  const stoppingStatusPollActive = ref(false);
  const refreshSourcesDone = ref(0);
  const refreshBusy = ref(false);

  const headlineTone = computed<"normal" | "good" | "warn" | "bad">(() => {
    if (!active.value && !stopping.value && state.value === "idle") return "normal";
    const s = state.value.toLowerCase();
    if (s === "error") return "bad";
    if (s === "dropping") return "warn";
    if (s === "stopped") return "normal";
    return "good";
  });

  const headlineText = computed(() => {
    if (state.value === "idle") return locale.t("status.disconnected");
    const key = streamStateKey(state.value);
    const label = key ? locale.t(key) : state.value;
    return detail.value ? `${label}: ${detail.value}` : label;
  });

  /* 帧预算按用户当前 FPS 自适应：1000/fps * 0.9。10FPS→90ms、25FPS→36ms、30FPS→30ms。 */
  const frameBudgetMs = computed(() => {
    const fps = settings.fps > 0 ? settings.fps : 20;
    return Math.floor((1000 / fps) * STAGE_MARGIN);
  });

  function windowMax(window: number[]): number {
    let m = 0;
    for (const v of window) if (v > m) m = v;
    return m;
  }

  /* 窗口未满（流刚启动）时退化为即时判定，保证启动初期即有反馈。 */
  const captureSlow = computed(() => windowMax(captureWindow.value) > frameBudgetMs.value);
  const encodeSlow = computed(() => windowMax(encodeWindow.value) > frameBudgetMs.value);
  const sendSlow = computed(() => windowMax(sendWindow.value) > frameBudgetMs.value);
  const deviceSlow = computed(() => windowMax(deviceWindow.value) > frameBudgetMs.value);

  const performanceLimited = computed(
    () => captureSlow.value || encodeSlow.value || sendSlow.value || deviceSlow.value,
  );

  function clearPreview() {
    previewSrc.value = "";
    previewReady.value = false;
  }

  function setTarget(value: StreamTarget | null) {
    target.value = value;
  }

  function clearMetrics() {
    metrics.value = {
      videoTransport: "none",
      fps: 0,
      jpegBytes: 0,
      jpegQualityUsed: 0,
      sourceScalePercent: 0,
      captureWaitMs: 0,
      captureProcessMs: 0,
      captureEncodeMs: 0,
      sendMs: 0,
      jpegEncodeAttempts: 0,
      deviceMs: 0,
      deviceReplacedFrames: 0,
      droppedFrames: 0,
    };
    captureWindow.value = [];
    encodeWindow.value = [];
    sendWindow.value = [];
    deviceWindow.value = [];
  }

  function applyMetrics(payload: StreamEvent) {
    metrics.value = {
      videoTransport: payload.videoTransport,
      fps: payload.fps,
      jpegBytes: payload.jpegBytes,
      jpegQualityUsed: payload.jpegQualityUsed,
      sourceScalePercent: payload.sourceScalePercent,
      captureWaitMs: payload.captureWaitMs,
      captureProcessMs: payload.captureProcessMs,
      captureEncodeMs: payload.captureEncodeMs,
      sendMs: payload.sendMs,
      jpegEncodeAttempts: payload.jpegEncodeAttempts,
      deviceMs: payload.deviceMs,
      deviceReplacedFrames: payload.deviceReplacedFrames,
      droppedFrames: payload.droppedFrames,
    };
    pushWindow(captureWindow.value, Math.max(payload.captureWaitMs, payload.captureProcessMs));
    pushWindow(encodeWindow.value, payload.captureEncodeMs);
    pushWindow(sendWindow.value, payload.sendMs);
    pushWindow(deviceWindow.value, payload.deviceMs);
  }

  function applyStreamStatus(payload: StreamEvent) {
    if (payload.state === "idle") return;
    const s = payload.state.toLowerCase();
    const failed = s === "error";
    const warning = s === "dropping";
    const stopped = s === "stopped";
    const stoppingNow = s === "stopping";
    stopping.value = stoppingNow;
    active.value = !failed && !stopped;
    state.value = payload.state;
    detail.value = payload.detail;
    pillMode.value = "event";
    pillTone.value = failed ? "bad" : warning ? "warn" : active.value ? "active" : "muted";
    applyMetrics(payload);
    activeMode.value = payload.projectionMode;
  }

  function starting() {
    streamStarting.value = true;
    active.value = true;
    stopping.value = false;
    pillMode.value = "starting";
    pillTone.value = "active";
  }

  function stopBegin() {
    stopping.value = true;
    streamStarting.value = false;
    pillMode.value = "stopping";
    pillTone.value = "active";
  }

  function setStreaming() {
    active.value = true;
    stopping.value = false;
    streamStarting.value = false;
    pillMode.value = "streaming";
    pillTone.value = "active";
  }

  function setStreamError() {
    active.value = false;
    stopping.value = false;
    streamStarting.value = false;
    pillMode.value = "error";
    pillTone.value = "bad";
  }

  return {
    active,
    stopping,
    regionBusy,
    activeMode,
    state,
    detail,
    pillText,
    pillTone,
    metrics,
    previewSrc,
    previewReady,
    target,
    streamStarting,
    stoppingStatusPollActive,
    refreshSourcesDone,
    refreshBusy,
    headlineTone,
    headlineText,
    frameBudgetMs,
    performanceLimited,
    captureSlow,
    encodeSlow,
    sendSlow,
    deviceSlow,
    clearPreview,
    setTarget,
    clearMetrics,
    applyMetrics,
    applyStreamStatus,
    starting,
    stopBegin,
    setStreaming,
    setStreamError,
  };
});
