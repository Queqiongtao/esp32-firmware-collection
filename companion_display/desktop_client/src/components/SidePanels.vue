<script setup lang="ts">
import { ref } from "vue";
import { NCollapse, NCollapseItem } from "naive-ui";
import { useSettingsStore } from "../stores/settings";
import DevicePanel from "./DevicePanel.vue";
import SourcePanel from "./SourcePanel.vue";
import StreamPanel from "./StreamPanel.vue";
import type { CaptureSource } from "../types/tauri";
import type { DisplayOrientation } from "../types/tauri";
import type { ProjectionMode } from "../types/tauri";
import { useLocaleStore } from "../stores/locale";
import { useLogsStore } from "../stores/logs";

const props = defineProps<{
  sourceId: string;
  sources: CaptureSource[];
  sourceLabel: (item: CaptureSource) => string;
  locked: boolean;
  readyLabel: string;
  readyTone: "good" | "warn" | "active";
  startDisabled: boolean;
  stopDisabled: boolean;
  active: boolean;
  regionBusy: boolean;
}>();

const emit = defineEmits<{
  (e: "update:sourceId", v: string): void;
  (e: "refresh"): void;
  (e: "check"): void;
  (e: "update:videoTransport", v: string): void;
  (e: "update:displayOrientation", v: DisplayOrientation): void;
  (e: "update:fps", v: number): void;
  (e: "update:quality", v: number): void;
  (e: "input-params"): void;
  (e: "start"): void;
  (e: "stop"): void;
  (e: "update:projectionMode", v: ProjectionMode): void;
  (e: "input-endpoint"): void;
  (e: "input-token"): void;
}>();

const settings = useSettingsStore();
const locale = useLocaleStore();
const logs = useLogsStore();

function forgetPortalSelection() {
  settings.clearPortalRestoreToken();
  logs.pushKey("log.portalSelectionCleared", "normal");
}
</script>

<template>
  <aside class="side">
    <n-collapse :default-expanded-names="['stream']" arrow-placement="left">
      <n-collapse-item :title="locale.t('panel.device')" name="device">
        <DevicePanel
          :locked="props.locked"
          :endpoint="settings.endpoint"
          :token="settings.token"
          @update:endpoint="(v) => (settings.endpoint = v)"
          @update:token="(v) => (settings.token = v)"
          @blur-endpoint="settings.normalizeEndpointInPlace()"
          @input-endpoint="emit('input-endpoint')"
          @input-token="emit('input-token')"
          @check="emit('check')"
        />
      </n-collapse-item>
      <n-collapse-item :title="locale.t('panel.source')" name="source">
        <SourcePanel
          :locked="props.locked"
          :sourceId="props.sourceId"
          :sources="props.sources"
          :sourceLabel="props.sourceLabel"
          :portalRemembered="Boolean(settings.portalRestoreToken)"
          @update:sourceId="(v) => (settings.sourceId = v)"
          @refresh="emit('refresh')"
          @forgetPortal="forgetPortalSelection"
        />
      </n-collapse-item>
      <n-collapse-item :title="locale.t('panel.stream')" name="stream">
        <StreamPanel
          :locked="props.locked"
          :videoTransport="settings.videoTransport"
          :displayOrientation="settings.displayOrientation"
          :fps="settings.fps"
          :quality="settings.quality"
          :readyLabel="props.readyLabel"
          :readyTone="props.readyTone"
          :startDisabled="props.startDisabled"
          :stopDisabled="props.stopDisabled"
          :projectionMode="settings.projectionMode"
          :active="props.active"
          :regionBusy="props.regionBusy"
          :touchControl="settings.touchControl"
          @update:videoTransport="(v) => emit('update:videoTransport', v)"
          @update:touchControl="(v) => (settings.touchControl = v)"
          @update:displayOrientation="(v) => (settings.displayOrientation = v)"
          @update:fps="(v) => (settings.fps = v)"
          @update:quality="(v) => (settings.quality = v)"
          @input-params="emit('input-params')"
          @start="emit('start')"
          @stop="emit('stop')"
          @update:projectionMode="(v) => emit('update:projectionMode', v)"
        />
      </n-collapse-item>
    </n-collapse>
  </aside>
</template>

<style scoped>
.side {
  display: flex;
  flex-direction: column;
  gap: 8px;
  min-width: 0;
  overflow-y: auto;
}

@media (max-width: 1100px) {
  .side {
    overflow-y: visible;
  }
}
</style>
