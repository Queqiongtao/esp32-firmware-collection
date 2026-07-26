<script setup lang="ts">
import { NButton, NSelect, NInputNumber, NRadioButton, NRadioGroup, NSwitch } from "naive-ui";
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
    <n-radio-group :value="projectionMode" :disabled="active || regionBusy" @update:value="(v: ProjectionMode) => emit('update:projectionMode', v)">
      <n-radio-button value="live">{{ locale.t("stream.mode.live") }}</n-radio-button>
      <n-radio-button value="region">{{ locale.t("stream.mode.region") }}</n-radio-button>
    </n-radio-group>
    <div class="ready-row">
      <span :class="['ready-label', `tone-${readyTone}`]">{{ readyLabel }}</span>
    </div>
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
      <div class="field">
        <label>FPS</label>
        <n-input-number
          :value="fps"
          :min="1"
          :max="30"
          :disabled="false"
          :show-button="false"
          @update:value="(v: number | null) => emit('update:fps', v ?? 25)"
          @input="() => emit('input-params')"
        />
      </div>
      <div class="field">
        <label>{{ locale.t("stream.jpegQuality") }}</label>
        <n-input-number
          :value="quality"
          :min="30"
          :max="95"
          :disabled="false"
          :show-button="false"
          @update:value="(v: number | null) => emit('update:quality', v ?? 70)"
          @input="() => emit('input-params')"
        />
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
    <div class="actions">
      <n-button type="primary" attr-type="button" :loading="regionBusy" :disabled="startDisabled" @click="emit('start')">
        {{ locale.t(projectionMode === "region" ? (active ? "stream.reselectRegion" : "stream.regionStart") : "stream.start") }}
      </n-button>
      <n-button type="error" attr-type="button" :disabled="stopDisabled" @click="emit('stop')">{{ locale.t("stream.stop") }}</n-button>
    </div>
  </div>
</template>

<style scoped>
.panel-content {
  display: grid;
  gap: 12px;
}

.ready-row {
  display: flex;
  justify-content: flex-end;
}

.ready-label {
  display: inline-flex;
  align-items: center;
  min-height: 26px;
  border-radius: 999px;
  padding: 4px 12px;
  font-size: 12px;
  font-weight: 700;
  white-space: nowrap;
}

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
  display: grid;
  gap: 10px;
}
</style>
