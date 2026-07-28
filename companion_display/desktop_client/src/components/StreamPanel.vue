<script setup lang="ts">
import { NButton, NSelect, NInputNumber, NRadioButton, NRadioGroup, NSlider, NSwitch } from "naive-ui";
import type { DisplayOrientation, ProjectionMode } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";
import { computed } from "vue";

const locale = useLocaleStore();

const props = defineProps<{
  locked: boolean;
  videoTransport: string;
  displayOrientation: DisplayOrientation;
  fps: number;
  quality: number;
  readyLabel: string;
  readyTone: "good" | "warn" | "active";
  startDisabled: boolean;
  stopDisabled: boolean;
  projectionMode: ProjectionMode;
  active: boolean;
  regionBusy: boolean;
  touchControl: boolean;
}>();

const emit = defineEmits<{
  (e: "update:videoTransport", v: string): void;
  (e: "update:displayOrientation", v: DisplayOrientation): void;
  (e: "update:fps", v: number): void;
  (e: "update:quality", v: number): void;
  (e: "input-params"): void;
  (e: "start"): void;
  (e: "stop"): void;
  (e: "update:projectionMode", v: ProjectionMode): void;
  (e: "update:touchControl", v: boolean): void;
}>();

const transportOptions = computed(() => [
  { label: locale.t("stream.tcp"), value: "tcp" },
  { label: locale.t("stream.udp"), value: "udp" },
]);

const orientationOptions = computed<Array<{ label: string; value: DisplayOrientation }>>(() => [
  { label: locale.t("stream.orientation.portrait"), value: "portrait" },
  { label: locale.t("stream.orientation.landscape_left"), value: "landscape_left" },
  { label: locale.t("stream.orientation.portrait_inverted"), value: "portrait_inverted" },
  { label: locale.t("stream.orientation.landscape_right"), value: "landscape_right" },
]);
</script>

<template>
  <div class="panel-content">
    <n-radio-group class="mode-control" :value="projectionMode" :disabled="active || regionBusy" @update:value="(v: ProjectionMode) => emit('update:projectionMode', v)">
      <n-radio-button value="live">{{ locale.t("stream.mode.live") }}</n-radio-button>
      <n-radio-button value="region">{{ locale.t("stream.mode.region") }}</n-radio-button>
    </n-radio-group>
    <div class="control-grid">
      <div class="field">
        <label>{{ locale.t("stream.transport") }}</label>
        <n-select
          :value="videoTransport"
          :options="transportOptions"
          :disabled="locked"
          @update:value="(v: string) => emit('update:videoTransport', v)"
        />
      </div>
      <div class="field">
        <label>{{ locale.t("stream.orientation") }}</label>
        <n-select
          :value="displayOrientation"
          :options="orientationOptions"
          :disabled="locked"
          @update:value="(v: DisplayOrientation) => emit('update:displayOrientation', v)"
        />
      </div>
      <div class="field range-field">
        <label>FPS</label>
        <div class="range-control">
          <n-slider :value="fps" :min="1" :max="30" :step="1" @update:value="(v: number) => { emit('update:fps', v); emit('input-params'); }" />
          <n-input-number :value="fps" :min="1" :max="30" size="small" @update:value="(v: number | null) => { emit('update:fps', v ?? 20); emit('input-params'); }" />
        </div>
      </div>
      <div class="field range-field">
        <label>{{ locale.t("stream.jpegQuality") }}</label>
        <div class="range-control">
          <n-slider :value="quality" :min="30" :max="95" :step="1" @update:value="(v: number) => { emit('update:quality', v); emit('input-params'); }" />
          <n-input-number :value="quality" :min="30" :max="95" size="small" @update:value="(v: number | null) => { emit('update:quality', v ?? 85); emit('input-params'); }" />
        </div>
      </div>
    </div>
    <div class="switch-row">
      <label>{{ locale.t("stream.touchControl") }}</label>
      <n-switch
        :value="touchControl"
        :disabled="locked"
        @update:value="(v: boolean) => emit('update:touchControl', v)"
      />
    </div>
    <div class="command-area">
      <div :class="['ready-row', `tone-${readyTone}`]">
        <span class="ready-dot" aria-hidden="true"></span>
        <span>{{ readyLabel }}</span>
      </div>
      <div class="actions">
        <n-button v-if="!active" type="primary" size="large" attr-type="button" :loading="regionBusy" :disabled="startDisabled" @click="emit('start')">
          {{ locale.t(projectionMode === "region" ? "stream.regionStart" : "stream.start") }}
        </n-button>
        <template v-else>
          <n-button v-if="projectionMode === 'region'" size="large" attr-type="button" :loading="regionBusy" :disabled="startDisabled" @click="emit('start')">
            {{ locale.t("stream.reselectRegion") }}
          </n-button>
          <n-button type="error" size="large" attr-type="button" :disabled="stopDisabled" @click="emit('stop')">{{ locale.t("stream.stop") }}</n-button>
        </template>
      </div>
    </div>
  </div>
</template>

<style scoped>
.panel-content {
  display: grid;
  gap: 12px;
}

.mode-control { width: 100%; }
.mode-control :deep(.n-radio-button) { width: 50%; text-align: center; }

.ready-row {
  display: grid;
  grid-template-columns: 8px minmax(0, 1fr);
  align-items: center;
  gap: 8px;
  min-height: 34px;
  border-radius: var(--cd-radius-sm);
  padding: 6px 9px;
  font-size: 12px;
  font-weight: 700;
  overflow-wrap: anywhere;
}

.ready-dot { width: 8px; height: 8px; border-radius: 50%; background: currentColor; }

.tone-good {
  background: var(--cd-good-soft);
  color: var(--cd-good);
}
.tone-warn {
  background: var(--cd-warn-soft);
  color: var(--cd-warn);
}
.tone-active {
  background: var(--cd-accent-soft);
  color: var(--cd-accent-ink);
}

.control-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
}

.range-field { grid-column: 1 / -1; }
.range-control {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 86px;
  gap: 12px;
  align-items: center;
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

.switch-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.switch-row label {
  color: var(--cd-subtle);
  font-size: 12px;
  font-weight: 700;
}

.actions {
  display: flex;
  gap: 8px;
}

.actions :deep(.n-button) { flex: 1 1 0; }

.command-area {
  display: grid;
  gap: 8px;
  padding-top: 12px;
  border-top: 1px solid var(--cd-border-soft);
}

@media (max-width: 420px) {
  .control-grid { grid-template-columns: minmax(0, 1fr); }
  .range-field { grid-column: auto; }
  .actions { flex-direction: column; }
}
</style>
