<script setup lang="ts">
import { ref } from "vue";
import { NButton, NInput } from "naive-ui";
import { useDeviceStore } from "../stores/device";
import { useLocaleStore } from "../stores/locale";

const device = useDeviceStore();
const locale = useLocaleStore();
const detailsOpen = ref(false);

defineProps<{
  locked: boolean;
  endpoint: string;
  token: string;
}>();

const emit = defineEmits<{
  (e: "update:endpoint", v: string): void;
  (e: "update:token", v: string): void;
  (e: "blur-endpoint"): void;
  (e: "input-endpoint"): void;
  (e: "input-token"): void;
  (e: "check"): void;
}>();
</script>

<template>
  <div class="panel-content">
    <div class="field-stack">
      <div class="field">
        <label>{{ locale.t("device.address") }}</label>
        <div class="address-row">
          <n-input
            :value="endpoint"
            :disabled="locked || device.checking"
            placeholder="ws://192.168.1.42/stream"
            autocomplete="off"
            spellcheck="false"
            @update:value="(v) => emit('update:endpoint', v)"
            @blur="() => emit('blur-endpoint')"
            @input="() => emit('input-endpoint')"
          />
          <n-button :loading="device.checking" :disabled="locked" @click="emit('check')">{{ locale.t("device.check") }}</n-button>
        </div>
      </div>
      <div class="field">
        <label>{{ locale.t("device.pairingCode") }}</label>
        <n-input
          :value="token"
          :disabled="locked || device.checking"
          inputmode="numeric"
          autocomplete="one-time-code"
          :placeholder="locale.t('device.pairingPlaceholder')"
          @update:value="(v) => emit('update:token', v)"
          @input="() => emit('input-token')"
        />
      </div>
    </div>
    <div :class="['device-summary', `tone-${device.devicePillTone}`]">
      <span class="status-dot" aria-hidden="true"></span>
      <span>{{ device.statusLine }}</span>
      <n-button text size="tiny" @click="detailsOpen = !detailsOpen">
        {{ locale.t(detailsOpen ? "device.hideDetails" : "device.showDetails") }}
      </n-button>
    </div>
    <dl v-if="detailsOpen" class="facts">
      <div><dt>{{ locale.t("device.connection") }}</dt><dd>{{ device.connection }}</dd></div>
      <div><dt>{{ locale.t("device.wifi") }}</dt><dd>{{ device.wifi }}</dd></div>
      <div><dt>{{ locale.t("device.screen") }}</dt><dd>{{ device.screen }}</dd></div>
      <div><dt>{{ locale.t("device.transport") }}</dt><dd>{{ device.transport }}</dd></div>
      <div><dt>{{ locale.t("device.touch") }}</dt><dd>{{ device.touch }}</dd></div>
    </dl>
  </div>
</template>

<style scoped>
.panel-content {
  display: grid;
  gap: 12px;
}

.field-stack {
  display: grid;
  gap: 12px;
}

.field label {
  display: block;
  margin-bottom: 6px;
  color: var(--cd-subtle);
  font-size: 12px;
  font-weight: 700;
}

.address-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 8px;
}

.device-summary {
  display: grid;
  grid-template-columns: 8px minmax(0, 1fr) auto;
  align-items: center;
  gap: 8px;
  min-height: 36px;
  padding: 7px 9px;
  border-radius: var(--cd-radius-sm);
  background: var(--cd-surface-2);
  color: var(--cd-muted);
  font-size: 12px;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--cd-subtle);
}

.tone-good .status-dot { background: var(--cd-good); }
.tone-warn .status-dot { background: var(--cd-warn); }
.tone-bad .status-dot { background: var(--cd-bad); }
.tone-active .status-dot { background: var(--cd-accent); }

.facts {
  display: grid;
  gap: 10px;
  margin: 4px 0 0;
}

.facts div {
  display: grid;
  grid-template-columns: 88px minmax(0, 1fr);
  gap: 10px;
  align-items: baseline;
  border-top: 1px solid var(--cd-border-hair);
  padding-top: 10px;
}

@media (max-width: 420px) {
  .address-row { grid-template-columns: minmax(0, 1fr); }
  .address-row :deep(.n-button) { width: 100%; }
}

dt {
  color: var(--cd-subtle);
  font-size: 12px;
  font-weight: 700;
}

dd {
  margin: 0;
  color: var(--cd-strong);
  overflow-wrap: anywhere;
  font-size: 13px;
}
</style>
