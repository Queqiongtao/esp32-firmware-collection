<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from "vue";
import { useStreamStore } from "../stores/stream";
import { useLocaleStore } from "../stores/locale";
import { useSettingsStore } from "../stores/settings";
import { useDeviceStore } from "../stores/device";

const stream = useStreamStore();
const locale = useLocaleStore();
const settings = useSettingsStore();
const device = useDeviceStore();
const stage = ref<HTMLElement | null>(null);
const availableWidth = ref(0);
const availableHeight = ref(0);

const landscape = computed(
  () => settings.displayOrientation === "landscape_left" || settings.displayOrientation === "landscape_right",
);
const physicalWidth = computed(() => stream.target?.width ?? device.info?.width ?? 368);
const physicalHeight = computed(() => stream.target?.height ?? device.info?.height ?? 448);
const previewAspect = computed(() =>
  landscape.value
    ? physicalHeight.value / physicalWidth.value
    : physicalWidth.value / physicalHeight.value,
);
const frameStyle = computed(() => {
  const width = availableWidth.value;
  const height = availableHeight.value;
  if (width <= 0 || height <= 0) return { width: "100%", height: "100%" };
  const containerAspect = width / height;
  const fittedWidth = containerAspect > previewAspect.value ? height * previewAspect.value : width;
  const fittedHeight = containerAspect > previewAspect.value ? height : width / previewAspect.value;
  return {
    width: `${Math.max(1, Math.floor(fittedWidth))}px`,
    height: `${Math.max(1, Math.floor(fittedHeight))}px`,
  };
});

const empty = computed(() => !stream.previewReady);
const limitHint = computed(() => stream.performanceLimited && stream.active);

let observer: ResizeObserver | null = null;

function measure() {
  const element = stage.value;
  if (!element) return;
  availableWidth.value = element.clientWidth;
  availableHeight.value = element.clientHeight;
}

onMounted(async () => {
  await nextTick();
  measure();
  if (typeof ResizeObserver !== "undefined" && stage.value) {
    observer = new ResizeObserver(measure);
    observer.observe(stage.value);
  }
});

onBeforeUnmount(() => observer?.disconnect());
</script>

<template>
  <div ref="stage" class="stage">
    <div class="frame" :style="frameStyle">
      <img v-if="!empty" :src="stream.previewSrc" :alt="locale.t('preview.alt')" />
      <div v-else class="placeholder">
        <div class="placeholder-title">{{ locale.t("preview.title") }}</div>
        <div class="placeholder-sub">{{ locale.t("preview.subtitle") }}</div>
      </div>
      <div v-if="limitHint" class="limit-badge">{{ locale.t("metric.limited") }}</div>
    </div>
  </div>
</template>

<style scoped>
.stage {
  width: 100%;
  min-width: 0;
  min-height: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100%;
}

.frame {
  position: relative;
  max-width: 100%;
  max-height: 100%;
  flex: 0 0 auto;
  border: 1px solid var(--cd-border);
  border-radius: var(--cd-radius);
  background: var(--cd-night);
  overflow: hidden;
  display: grid;
  place-items: center;
}

.frame img {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
}

.placeholder {
  color: var(--cd-night-fg);
  text-align: center;
  padding: 0 16px;
}

.placeholder-title {
  font-size: 14px;
  font-weight: 700;
  margin-bottom: 6px;
}

.placeholder-sub {
  font-size: 12px;
}

.limit-badge {
  position: absolute;
  top: 10px;
  right: 10px;
  background: var(--cd-warn-soft);
  color: var(--cd-warn);
  font-size: 12px;
  font-weight: 700;
  border-radius: 999px;
  padding: 4px 12px;
}
</style>
