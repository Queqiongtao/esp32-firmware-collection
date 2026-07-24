<script setup lang="ts">
import { useStreamStore } from "../stores/stream";

const stream = useStreamStore();

const items = [
  { key: "transport", name: "Transport", get: () => (stream.metrics.videoTransport === "none" ? "--" : stream.metrics.videoTransport.toUpperCase()) },
  { key: "fps", name: "FPS", get: () => String(stream.metrics.fps) },
  { key: "capture", name: "Capture", get: () => `${stream.metrics.captureWaitMs} ms`, slow: () => stream.captureSlow },
  { key: "encode", name: "Encode", get: () => `${stream.metrics.captureEncodeMs} ms`, slow: () => stream.encodeSlow },
  { key: "device", name: "Device", get: () => `${stream.metrics.deviceMs} ms`, slow: () => stream.deviceSlow },
  { key: "replaced", name: "Replaced", get: () => String(stream.metrics.deviceReplacedFrames) },
  { key: "dropped", name: "Dropped", get: () => String(stream.metrics.droppedFrames) },
];
</script>

<template>
  <div class="metric-row">
    <div v-for="item in items" :key="item.key" class="metric" :class="{ slow: 'slow' in item && item.slow?.() }">
      <span class="metric-name">{{ item.name }}</span>
      <span class="metric-value">{{ item.get() }}</span>
    </div>
    <span class="metric limited" :class="{ hidden: !stream.performanceLimited || !stream.active }">Performance limited</span>
  </div>
</template>

<style scoped>
.metric-row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  margin-bottom: 14px;
}

.metric {
  display: grid;
  gap: 2px;
  min-width: 84px;
  border: 1px solid var(--cd-border-soft);
  border-radius: var(--cd-radius-sm);
  background: var(--cd-surface-2);
  padding: 6px 10px;
  text-align: center;
}

.metric.limited {
  display: inline-flex;
  align-items: center;
  background: var(--cd-warn-soft);
  color: var(--cd-warn);
  border-color: transparent;
  min-width: 0;
  font-size: 12px;
  font-weight: 700;
  padding: 4px 12px;
  border-radius: 999px;
}

.metric.limited.hidden {
  display: none;
}

.metric-name {
  color: var(--cd-subtle);
  font-size: 11px;
  font-weight: 700;
  text-transform: uppercase;
}

.metric-value {
  color: var(--cd-strong);
  font-family: var(--cd-mono);
  font-size: 13px;
  font-weight: 650;
}

.metric.slow .metric-value {
  color: var(--cd-bad);
}
</style>