<script setup lang="ts">
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
    <div class="side-heading">
      <div>
        <h2>{{ locale.t("workspace.setup") }}</h2>
        <p>{{ locale.t("workspace.setupHint") }}</p>
      </div>
    </div>
    <div class="side-scroll">
      <section class="setup-section">
        <header class="section-heading">
          <span class="step-number">1</span>
          <div><h3>{{ locale.t("panel.device") }}</h3><p>{{ locale.t("panel.deviceHint") }}</p></div>
        </header>
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
      </section>
      <section class="setup-section">
        <header class="section-heading">
          <span class="step-number">2</span>
          <div><h3>{{ locale.t("panel.source") }}</h3><p>{{ locale.t("panel.sourceHint") }}</p></div>
        </header>
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
      </section>
      <section class="setup-section stream-section">
        <header class="section-heading">
          <span class="step-number">3</span>
          <div><h3>{{ locale.t("panel.stream") }}</h3><p>{{ locale.t("panel.streamHint") }}</p></div>
        </header>
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
      </section>
    </div>
  </aside>
</template>

<style scoped>
.side {
  display: flex;
  flex-direction: column;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
  border: 1px solid var(--cd-border);
  border-radius: var(--cd-radius);
  background: var(--cd-surface);
}

.side-heading {
  padding: 12px 14px;
  border-bottom: 1px solid var(--cd-border-soft);
}

.side-heading h2 {
  color: var(--cd-strong);
  font-size: 15px;
}

.side-heading p,
.section-heading p {
  margin-top: 3px;
  color: var(--cd-muted);
  font-size: 12px;
}

.side-scroll {
  min-height: 0;
  overflow-y: auto;
  padding: 0 14px 14px;
}

.setup-section {
  padding: 16px 0;
  border-bottom: 1px solid var(--cd-border-soft);
}

.setup-section:last-child { border-bottom: 0; }

.section-heading {
  display: grid;
  grid-template-columns: 24px minmax(0, 1fr);
  gap: 9px;
  align-items: start;
  margin-bottom: 13px;
}

.section-heading h3 {
  margin: 0;
  color: var(--cd-strong);
  font-size: 14px;
  line-height: 1.3;
}

.step-number {
  display: grid;
  place-items: center;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: var(--cd-accent-soft);
  color: var(--cd-accent-ink);
  font-size: 12px;
  font-weight: 800;
}

@media (max-width: 1100px) {
  .side {
    overflow: visible;
  }
  .side-scroll { overflow: visible; }
}
</style>
