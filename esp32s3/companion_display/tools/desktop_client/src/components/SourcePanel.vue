<script setup lang="ts">
import { computed } from "vue";
import { NButton, NSelect, NInputNumber, NSwitch } from "naive-ui";
import type { CaptureSource } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";

const locale = useLocaleStore();

const props = defineProps<{
  locked: boolean;
  sourceId: string;
  cropEnabled: boolean;
  regionX: number;
  regionY: number;
  regionWidth: number;
  regionHeight: number;
  sources: CaptureSource[];
  sourceLabel: (item: CaptureSource) => string;
}>();

const emit = defineEmits<{
  (e: "update:sourceId", v: string): void;
  (e: "update:cropEnabled", v: boolean): void;
  (e: "update:regionX", v: number): void;
  (e: "update:regionY", v: number): void;
  (e: "update:regionWidth", v: number): void;
  (e: "update:regionHeight", v: number): void;
  (e: "refresh"): void;
}>();

const options = computed(() =>
  props.sources.length
    ? props.sources.map((s) => ({ label: props.sourceLabel(s), value: s.id }))
    : [{ label: locale.t("source.noneAvailable"), value: "" }],
);

const selected = computed(() => props.sources.find((s) => s.id === props.sourceId) ?? null);

const summary = computed(() => {
  const item = selected.value;
  if (!item) return locale.t("source.noneSelected");
  if (item.id === "test:color-bars") return locale.t("source.colorBarsSummary");
  if (item.id === "linux:portal") return locale.t("source.portalSummary");
  const kindKeys = {
    test: "source.kind.test",
    portal: "source.kind.portal",
    monitor: "source.kind.monitor",
    window: "source.kind.window",
  } as const;
  const kind = item.kind in kindKeys ? locale.t(kindKeys[item.kind as keyof typeof kindKeys]) : item.kind;
  return locale.t("source.selectedSummary", { kind, width: item.width, height: item.height });
});
</script>

<template>
  <div class="panel-content">
    <div class="field">
      <label>{{ locale.t("source.capture") }}</label>
      <n-select
        :value="sourceId"
        :options="options"
        :disabled="locked"
        @update:value="(v: string) => emit('update:sourceId', v)"
      />
      <n-button size="small" :disabled="locked" class="refresh-btn" @click="emit('refresh')">{{ locale.t("source.refresh") }}</n-button>
    </div>
    <div class="source-summary">{{ summary }}</div>
    <div class="toggle-row">
      <n-switch
        :value="cropEnabled"
        :disabled="locked"
        @update:value="(v: boolean) => emit('update:cropEnabled', v)"
      />
      <span>{{ locale.t("source.crop") }}</span>
    </div>
    <div v-if="cropEnabled" class="region">
      <div class="field">
        <label>{{ locale.t("source.x") }}</label>
        <n-input-number :value="regionX" :min="0" :disabled="locked" :show-button="false" @update:value="(v: number | null) => emit('update:regionX', v ?? 0)" />
      </div>
      <div class="field">
        <label>{{ locale.t("source.y") }}</label>
        <n-input-number :value="regionY" :min="0" :disabled="locked" :show-button="false" @update:value="(v: number | null) => emit('update:regionY', v ?? 0)" />
      </div>
      <div class="field">
        <label>{{ locale.t("source.width") }}</label>
        <n-input-number :value="regionWidth" :min="0" :disabled="locked" :show-button="false" @update:value="(v: number | null) => emit('update:regionWidth', v ?? 0)" />
      </div>
      <div class="field">
        <label>{{ locale.t("source.height") }}</label>
        <n-input-number :value="regionHeight" :min="0" :disabled="locked" :show-button="false" @update:value="(v: number | null) => emit('update:regionHeight', v ?? 0)" />
      </div>
    </div>
  </div>
</template>

<style scoped>
.panel-content {
  display: grid;
  gap: 12px;
}

.field {
  display: grid;
  gap: 6px;
}

.field label {
  color: var(--cd-subtle);
  font-size: 12px;
  font-weight: 700;
}

.refresh-btn {
  margin-top: 6px;
  justify-self: start;
}

.source-summary {
  border: 1px solid var(--cd-border-soft);
  border-radius: var(--cd-radius-sm);
  background: var(--cd-surface-2);
  color: var(--cd-muted);
  padding: 10px;
  font-size: 12px;
  line-height: var(--cd-line);
}

.toggle-row {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 12px;
  color: var(--cd-strong);
}

.region {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 10px;
}
</style>
