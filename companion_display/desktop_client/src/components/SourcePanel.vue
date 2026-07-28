<script setup lang="ts">
import { computed } from "vue";
import { NButton, NSelect } from "naive-ui";
import type { CaptureSource } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";

const locale = useLocaleStore();

const props = defineProps<{
  locked: boolean;
  sourceId: string;
  sources: CaptureSource[];
  sourceLabel: (item: CaptureSource) => string;
  portalRemembered: boolean;
}>();

const emit = defineEmits<{
  (e: "update:sourceId", v: string): void;
  (e: "refresh"): void;
  (e: "forgetPortal"): void;
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
      <div class="source-row">
        <n-select
          :value="sourceId"
          :options="options"
          :disabled="locked"
          @update:value="(v: string) => emit('update:sourceId', v)"
        />
        <n-button :disabled="locked" :title="locale.t('source.refresh')" @click="emit('refresh')">
          <span class="refresh-icon" aria-hidden="true">↻</span>
          <span class="refresh-label">{{ locale.t("source.refresh") }}</span>
        </n-button>
      </div>
    </div>
    <div class="source-summary">{{ summary }}</div>
    <div v-if="sourceId === 'linux:portal'" class="portal-memory">
      <span>{{ locale.t(portalRemembered ? "source.portalRemembered" : "source.portalWillPrompt") }}</span>
      <n-button v-if="portalRemembered" size="tiny" secondary :disabled="locked" @click="emit('forgetPortal')">
        {{ locale.t("source.portalReselect") }}
      </n-button>
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

.source-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 8px;
}

.refresh-icon { font-size: 17px; line-height: 1; }
.refresh-label { display: none; }

@media (max-width: 420px) {
  .source-row { grid-template-columns: minmax(0, 1fr); }
  .refresh-label { display: inline; margin-left: 5px; }
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

.portal-memory {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  color: var(--cd-muted);
  font-size: 12px;
}

</style>
