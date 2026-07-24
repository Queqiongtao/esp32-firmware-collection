<script setup lang="ts">
import { NButton, NInput } from "naive-ui";
import { useDeviceStore } from "../stores/device";
import { useLocaleStore } from "../stores/locale";

const device = useDeviceStore();
const locale = useLocaleStore();

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
    <div class="actions">
      <n-button size="small" :loading="device.checking" :disabled="locked" @click="emit('check')">{{ locale.t("device.check") }}</n-button>
    </div>
    <dl class="facts">
      <div><dt>{{ locale.t("device.connection") }}</dt><dd>{{ device.connection }}</dd></div>
      <div><dt>{{ locale.t("device.wifi") }}</dt><dd>{{ device.wifi }}</dd></div>
      <div><dt>{{ locale.t("device.screen") }}</dt><dd>{{ device.screen }}</dd></div>
      <div><dt>{{ locale.t("device.transport") }}</dt><dd>{{ device.transport }}</dd></div>
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

.actions {
  display: flex;
  justify-content: flex-end;
}

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
