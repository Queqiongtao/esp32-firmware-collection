<script setup lang="ts">
import { NButton, NSelect, NInputNumber } from "naive-ui";

defineProps<{
  locked: boolean;
  videoTransport: string;
  fps: number;
  quality: number;
  readyLabel: string;
  readyTone: "good" | "warn" | "active";
  startDisabled: boolean;
  stopDisabled: boolean;
}>();

const emit = defineEmits<{
  (e: "update:videoTransport", v: string): void;
  (e: "update:fps", v: number): void;
  (e: "update:quality", v: number): void;
  (e: "input-params"): void;
  (e: "start"): void;
  (e: "stop"): void;
}>();

const transportOptions = [
  { label: "TCP (reliable)", value: "tcp" },
  { label: "UDP (low latency)", value: "udp" },
];
</script>

<template>
  <div class="panel-content">
    <div class="ready-row">
      <span :class="['ready-label', `tone-${readyTone}`]">{{ readyLabel }}</span>
    </div>
    <div class="control-grid">
      <div class="field">
        <label>Video transport</label>
        <n-select
          :value="videoTransport"
          :options="transportOptions"
          :disabled="locked"
          @update:value="(v: string) => emit('update:videoTransport', v)"
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
        <label>JPEG quality</label>
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
    <div class="actions">
      <n-button type="primary" attr-type="button" :disabled="startDisabled" @click="emit('start')">
        Start Wi-Fi Stream
      </n-button>
      <n-button type="error" attr-type="button" :disabled="stopDisabled" @click="emit('stop')">Stop</n-button>
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

.actions {
  display: grid;
  gap: 10px;
}
</style>