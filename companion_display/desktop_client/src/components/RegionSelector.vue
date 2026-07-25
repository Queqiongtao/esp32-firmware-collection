<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import { NButton } from "naive-ui";
import type { CaptureRegion, RegionSelectionPreview } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";

const props = defineProps<{ preview: RegionSelectionPreview }>();
const emit = defineEmits<{
  (e: "confirm", region: CaptureRegion): void;
  (e: "cancel"): void;
}>();
const locale = useLocaleStore();
const stage = ref<HTMLElement | null>(null);
const start = ref<{ x: number; y: number } | null>(null);
const end = ref<{ x: number; y: number } | null>(null);
const dragging = ref(false);
const viewportWidth = ref(window.innerWidth);
const viewportHeight = ref(window.innerHeight);

const imageRect = computed(() => {
  const vw = viewportWidth.value;
  const vh = viewportHeight.value;
  const scale = Math.min(vw / props.preview.width, vh / props.preview.height);
  const width = props.preview.width * scale;
  const height = props.preview.height * scale;
  return { left: (vw - width) / 2, top: (vh - height) / 2, width, height, scale };
});

const contentRect = computed(() => {
  const image = imageRect.value;
  return {
    left: image.left + props.preview.contentX * image.scale,
    top: image.top + props.preview.contentY * image.scale,
    width: props.preview.contentWidth * image.scale,
    height: props.preview.contentHeight * image.scale,
  };
});

const selectionRect = computed(() => {
  if (!start.value || !end.value) return null;
  const left = Math.min(start.value.x, end.value.x);
  const top = Math.min(start.value.y, end.value.y);
  return { left, top, width: Math.abs(end.value.x - start.value.x), height: Math.abs(end.value.y - start.value.y) };
});

const sourceRegion = computed<CaptureRegion | null>(() => {
  const selection = selectionRect.value;
  const content = contentRect.value;
  if (!selection || selection.width < 1 || selection.height < 1) return null;
  const x = Math.round((selection.left - content.left) * props.preview.sourceWidth / content.width);
  const y = Math.round((selection.top - content.top) * props.preview.sourceHeight / content.height);
  const width = Math.round(selection.width * props.preview.sourceWidth / content.width);
  const height = Math.round(selection.height * props.preview.sourceHeight / content.height);
  return width >= 16 && height >= 16 ? { x, y, width, height } : null;
});

function clampPoint(event: PointerEvent) {
  const content = contentRect.value;
  return {
    x: Math.min(content.left + content.width, Math.max(content.left, event.clientX)),
    y: Math.min(content.top + content.height, Math.max(content.top, event.clientY)),
  };
}

function pointIsInsideContent(event: PointerEvent) {
  const content = contentRect.value;
  return event.clientX >= content.left
    && event.clientX <= content.left + content.width
    && event.clientY >= content.top
    && event.clientY <= content.top + content.height;
}

function pointerDown(event: PointerEvent) {
  if (!pointIsInsideContent(event)) return;
  dragging.value = true;
  start.value = clampPoint(event);
  end.value = start.value;
  stage.value?.setPointerCapture(event.pointerId);
}

function pointerMove(event: PointerEvent) {
  if (dragging.value && start.value) end.value = clampPoint(event);
}

function pointerUp(event: PointerEvent) {
  if (!dragging.value) return;
  if (start.value) end.value = clampPoint(event);
  dragging.value = false;
  if (stage.value?.hasPointerCapture(event.pointerId)) stage.value.releasePointerCapture(event.pointerId);
}

function stopDragging() {
  dragging.value = false;
}

function measureViewport() {
  viewportWidth.value = window.innerWidth;
  viewportHeight.value = window.innerHeight;
}

function keydown(event: KeyboardEvent) {
  if (event.key === "Escape") emit("cancel");
  if (event.key === "Enter" && sourceRegion.value) emit("confirm", sourceRegion.value);
}

onMounted(() => {
  measureViewport();
  window.addEventListener("keydown", keydown);
  window.addEventListener("resize", measureViewport);
  window.visualViewport?.addEventListener("resize", measureViewport);
});
onBeforeUnmount(() => {
  window.removeEventListener("keydown", keydown);
  window.removeEventListener("resize", measureViewport);
  window.visualViewport?.removeEventListener("resize", measureViewport);
});
</script>

<template>
  <div
    ref="stage"
    class="selector"
    @pointerdown="pointerDown"
    @pointermove="pointerMove"
    @pointerup="pointerUp"
    @pointercancel="stopDragging"
    @lostpointercapture="stopDragging"
  >
    <img
      :src="`data:image/jpeg;base64,${preview.jpegBase64}`"
      alt=""
      :style="{ left: `${imageRect.left}px`, top: `${imageRect.top}px`, width: `${imageRect.width}px`, height: `${imageRect.height}px` }"
    />
    <div class="shade" />
    <div
      v-if="selectionRect"
      class="selection"
      :style="{ left: `${selectionRect.left}px`, top: `${selectionRect.top}px`, width: `${selectionRect.width}px`, height: `${selectionRect.height}px` }"
    >
      <span v-if="sourceRegion">{{ sourceRegion.width }} x {{ sourceRegion.height }}</span>
    </div>
    <div class="actions" @pointerdown.stop @pointermove.stop @pointerup.stop>
      <n-button @click="emit('cancel')">{{ locale.t("region.cancel") }}</n-button>
      <n-button type="primary" :disabled="!sourceRegion" @click="sourceRegion && emit('confirm', sourceRegion)">
        {{ locale.t("region.confirm") }}
      </n-button>
    </div>
  </div>
</template>

<style scoped>
.selector { position: fixed; inset: 0; z-index: 10000; overflow: hidden; background: #050505; cursor: crosshair; user-select: none; touch-action: none; }
.selector img { position: absolute; object-fit: contain; pointer-events: none; }
.shade { position: absolute; inset: 0; background: rgba(0, 0, 0, 0.28); pointer-events: none; }
.selection { position: fixed; border: 2px solid #22c55e; background: rgba(34, 197, 94, 0.08); box-shadow: 0 0 0 9999px rgba(0, 0, 0, 0.42); pointer-events: none; }
.selection span { position: absolute; left: 0; top: -30px; padding: 4px 8px; background: #111; color: #fff; font: 12px sans-serif; white-space: nowrap; }
.actions { position: fixed; left: 50%; bottom: 28px; transform: translateX(-50%); display: flex; gap: 10px; padding: 8px; background: rgba(17, 17, 17, 0.9); border-radius: 6px; }
</style>
