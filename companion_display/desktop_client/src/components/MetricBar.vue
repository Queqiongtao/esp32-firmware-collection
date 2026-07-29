<script setup lang="ts">
import { useStreamStore } from "../stores/stream";
import { useLocaleStore } from "../stores/locale";
import { computed, ref } from "vue";

const stream = useStreamStore();
const locale = useLocaleStore();
const detailsOpen = ref(false);

const jpegMetric = computed(() => {
  if (!stream.metrics.jpegQualityUsed || !stream.target) return "--";
  const usedKiB = Math.ceil(stream.metrics.jpegBytes / 1024);
  const limitKiB = Math.floor((stream.target.maxFrameBytes - 24) / 1024);
  const attempts = stream.metrics.jpegEncodeAttempts ? ` ${stream.metrics.jpegEncodeAttempts}x` : "";
  return `Q${stream.metrics.jpegQualityUsed}${attempts} ${usedKiB}/${limitKiB}K`;
});

const primaryItems = computed(() => [
  { key: "transport", name: locale.t("metric.transport"), get: () => (stream.metrics.videoTransport === "none" ? "--" : stream.metrics.videoTransport.toUpperCase()) },
  { key: "fps", name: "FPS", get: () => String(stream.metrics.fps) },
  { key: "device", name: locale.t("metric.device"), get: () => `${stream.metrics.deviceMs} ms`, slow: () => stream.deviceSlow },
  { key: "dropped", name: locale.t("metric.dropped"), get: () => String(stream.metrics.droppedFrames) },
]);

const detailItems = computed(() => [
  { key: "udp", name: "UDP / FEC", get: () => (stream.metrics.udpFragmentBudget ? `${stream.metrics.udpFragmentBudget}F +${stream.metrics.udpFecRecoveredFragments}/-${stream.metrics.udpUnrecoverableFrames}` : "--") },
  { key: "udp-link", name: "UDP LINK", get: () => (stream.metrics.udpRateKbps ? `${(stream.metrics.udpRateKbps / 1000).toFixed(1)}M ${stream.metrics.udpDeliveryMs} ms` : "--") },
  { key: "scale", name: "SCALE", get: () => (stream.metrics.sourceScalePercent ? `${stream.metrics.sourceScalePercent}%` : "--") },
  { key: "jpeg", name: "JPEG", get: () => jpegMetric.value },
  { key: "capture", name: locale.t("metric.capture"), hint: locale.t("metric.captureHint"), get: () => `${stream.metrics.captureProcessMs}/${stream.metrics.captureWaitMs} ms`, slow: () => stream.captureSlow },
  { key: "encode", name: locale.t("metric.encode"), get: () => `${stream.metrics.captureEncodeMs} ms`, slow: () => stream.encodeSlow },
  { key: "send", name: locale.t("metric.send"), get: () => `${stream.metrics.sendMs} ms`, slow: () => stream.sendSlow },
  { key: "replaced", name: locale.t("metric.replaced"), get: () => String(stream.metrics.deviceReplacedFrames) },
]);
</script>

<template>
  <section class="metrics">
    <div class="metrics-toolbar">
      <span class="metrics-title">{{ locale.t("metric.runtime") }}</span>
      <button type="button" class="details-toggle" :aria-expanded="detailsOpen" @click="detailsOpen = !detailsOpen">
        {{ locale.t(detailsOpen ? "metric.hideDetails" : "metric.showDetails") }}
        <span aria-hidden="true">{{ detailsOpen ? "⌃" : "⌄" }}</span>
      </button>
    </div>
    <div class="metric-row primary">
      <div v-for="item in primaryItems" :key="item.key" class="metric" :class="{ slow: 'slow' in item && item.slow?.() }">
        <span class="metric-name">{{ item.name }}</span>
        <span class="metric-value">{{ item.get() }}</span>
      </div>
      <span v-if="stream.performanceLimited && stream.active" class="limited">{{ locale.t("metric.limited") }}</span>
    </div>
    <div v-if="detailsOpen" class="metric-row details">
      <div v-for="item in detailItems" :key="item.key" class="metric" :class="{ slow: 'slow' in item && item.slow?.() }" :title="'hint' in item ? item.hint : undefined">
        <span class="metric-name">{{ item.name }}</span>
        <span class="metric-value">{{ item.get() }}</span>
      </div>
    </div>
  </section>
</template>

<style scoped>
.metrics {
  border-top: 1px solid var(--cd-border-soft);
  padding: 10px 14px 12px;
  background: var(--cd-surface);
}

.metrics-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 8px;
}

.metrics-title { color: var(--cd-strong); font-size: 12px; font-weight: 700; }
.details-toggle {
  border: 0;
  padding: 2px 0;
  background: transparent;
  color: var(--cd-accent-ink);
  font: inherit;
  font-size: 12px;
  cursor: pointer;
}
.details-toggle:focus-visible { outline: 2px solid var(--cd-focus); outline-offset: 2px; }

.metric-row {
  display: grid;
  grid-template-columns: repeat(4, minmax(72px, 1fr));
  gap: 8px;
  align-items: stretch;
}

.metric-row.details {
  grid-template-columns: repeat(auto-fit, minmax(100px, 1fr));
  margin-top: 8px;
  padding-top: 8px;
  border-top: 1px solid var(--cd-border-hair);
}

.metric {
  display: grid;
  gap: 2px;
  min-width: 0;
  padding: 3px 8px 3px 0;
  border-right: 1px solid var(--cd-border-soft);
}

.metric-row .metric:last-of-type { border-right: 0; }

.limited {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  background: var(--cd-warn-soft);
  color: var(--cd-warn);
  font-size: 12px;
  font-weight: 700;
  padding: 4px 8px;
  border-radius: var(--cd-radius-sm);
}

.metric-name {
  color: var(--cd-subtle);
  font-size: 11px;
  font-weight: 700;
}

.metric-value {
  color: var(--cd-strong);
  font-family: var(--cd-mono);
  font-size: 13px;
  font-weight: 650;
  white-space: nowrap;
}

@media (max-width: 560px) {
  .metric-row { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .metric:nth-child(2n) { border-right: 0; }
}

.metric.slow .metric-value {
  color: var(--cd-bad);
}
</style>
