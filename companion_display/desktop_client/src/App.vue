<script setup lang="ts">
import { onMounted } from "vue";
import { NButton, NConfigProvider, NMessageProvider, NDialogProvider } from "naive-ui";
import TopBar from "./components/TopBar.vue";
import MetricBar from "./components/MetricBar.vue";
import PreviewStage from "./components/PreviewStage.vue";
import SidePanels from "./components/SidePanels.vue";
import LogPanel from "./components/LogPanel.vue";
import { useThemeStore } from "./stores/theme";
import { useSettingsStore } from "./stores/settings";
import { useDeviceStore } from "./stores/device";
import { useStreamStore } from "./stores/stream";
import { useLogsStore } from "./stores/logs";
import { useLocaleStore } from "./stores/locale";
import { useNaiveTheme } from "./theme/naivetheme";
import { useTauriEvents } from "./composables/useTauriEvents";
import { useStreamLifecycle } from "./composables/useStreamLifecycle";
import RegionSelector from "./components/RegionSelector.vue";

const theme = useThemeStore();
const settings = useSettingsStore();
const device = useDeviceStore();
const stream = useStreamStore();
const logs = useLogsStore();
const locale = useLocaleStore();
const { naive, overrides } = useNaiveTheme();
const events = useTauriEvents();
const lc = useStreamLifecycle();

onMounted(async () => {
  theme.bindMedia();
  theme.apply();
  settings.restore();
  device.checking = false;
  await events.init();
  await lc.refreshSources();
  lc.startPolling();
  logs.pushKey("log.clientReady", "good");
  if (settings.normalizedEndpoint && settings.token.length === 6) {
    lc.checkDeviceNow().catch(() => undefined);
  }
});

function onTransportChange(v: string) {
  settings.videoTransport = v;
}
</script>

<template>
  <n-config-provider :theme="naive" :theme-overrides="overrides">
    <n-message-provider>
      <n-dialog-provider>
        <div class="app">
          <TopBar />
          <div class="workspace">
            <main class="preview-workspace">
              <div class="preview-heading">
                <div>
                  <h2>{{ locale.t("workspace.preview") }}</h2>
                  <p>{{ locale.t("workspace.previewHint") }}</p>
                </div>
                <div class="preview-command">
                  <span :class="['workspace-status', `tone-${lc.readyTone.value}`]">{{ lc.readyLabel.value }}</span>
                  <n-button
                    v-if="!stream.active"
                    type="primary"
                    :loading="stream.regionBusy"
                    :disabled="lc.startDisabled.value"
                    @click="lc.start"
                  >
                    {{ locale.t(settings.projectionMode === "region" ? "stream.regionStart" : "stream.start") }}
                  </n-button>
                  <n-button v-else type="error" :disabled="lc.stopDisabled.value" @click="lc.stop">
                    {{ locale.t("stream.stop") }}
                  </n-button>
                </div>
              </div>
              <div class="preview-stage-wrap">
                <PreviewStage />
              </div>
              <MetricBar />
            </main>
            <SidePanels
              :sourceId="settings.sourceId"
              :sources="lc.sources.value"
              :sourceLabel="lc.sourceLabel"
              :locked="lc.locked.value"
              :readyLabel="lc.readyLabel.value"
              :readyTone="lc.readyTone.value"
              :startDisabled="lc.startDisabled.value"
              :stopDisabled="lc.stopDisabled.value"
              :active="stream.active"
              :regionBusy="stream.regionBusy"
              @update:sourceId="(v: string) => (settings.sourceId = v)"
              @refresh="lc.refreshSources"
              @check="lc.checkDeviceNow"
              @update:videoTransport="onTransportChange"
              @input-params="lc.scheduleParamsUpdate"
              @input-endpoint="lc.scheduleProbe"
              @input-token="lc.scheduleProbe"
              @start="lc.start"
              @stop="lc.stop"
              @update:projectionMode="(v) => (settings.projectionMode = v)"
            />
          </div>
          <LogPanel />
        </div>
        <RegionSelector
          v-if="lc.regionPreview.value"
          :preview="lc.regionPreview.value"
          :canChangeSource="lc.regionCanChangeSource.value"
          @confirm="lc.confirmRegion"
          @cancel="lc.cancelRegion"
          @change-source="lc.changeRegionSource"
        />
      </n-dialog-provider>
    </n-message-provider>
  </n-config-provider>
</template>

<style scoped>
.app {
  width: min(1480px, 100%);
  height: 100vh;
  margin: 0 auto;
  padding: 20px 22px;
  display: flex;
  flex-direction: column;
  min-height: 0;
  gap: 14px;
}

.workspace {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(360px, 400px);
  gap: 16px;
  min-height: 0;
  flex: 1 1 0;
}

.preview-workspace {
  min-width: 0;
  min-height: 0;
  display: grid;
  grid-template-rows: auto minmax(240px, 1fr) auto;
  border: 1px solid var(--cd-border);
  border-radius: var(--cd-radius);
  background: var(--cd-surface);
  overflow: hidden;
}

.preview-heading {
  min-width: 0;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 12px 14px;
  border-bottom: 1px solid var(--cd-border-soft);
}

.preview-heading h2 {
  color: var(--cd-strong);
  font-size: 15px;
  line-height: 1.3;
}

.preview-heading p {
  margin-top: 3px;
  color: var(--cd-muted);
  font-size: 12px;
}

.workspace-status {
  min-width: 0;
  padding: 4px 9px;
  border-radius: var(--cd-radius-sm);
  background: var(--cd-neutral-soft);
  color: var(--cd-neutral-ink);
  font-size: 12px;
  font-weight: 700;
  overflow-wrap: anywhere;
  text-align: right;
}

.preview-command {
  min-width: 0;
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
}

.preview-command :deep(.n-button) { flex: 0 0 auto; }

.workspace-status.tone-good { background: var(--cd-good-soft); color: var(--cd-good); }
.workspace-status.tone-warn { background: var(--cd-warn-soft); color: var(--cd-warn); }
.workspace-status.tone-active { background: var(--cd-accent-soft); color: var(--cd-accent-ink); }

.preview-stage-wrap {
  min-width: 0;
  min-height: 0;
  overflow: hidden;
  padding: 16px;
  background: var(--cd-surface-2);
}

@media (max-width: 1100px) {
  .app {
    height: auto;
    min-height: 100vh;
  }

  .workspace {
    grid-template-columns: minmax(0, 1fr);
    grid-template-rows: minmax(420px, 62vh) auto;
    flex: 0 0 auto;
  }
}

@media (max-width: 640px) {
  .app { padding: 12px; gap: 10px; }
  .preview-heading { align-items: flex-start; }
  .preview-heading p { display: none; }
  .preview-command { max-width: 68%; }
  .preview-stage-wrap { padding: 10px; }
}

@media (max-width: 460px) {
  .preview-heading { display: grid; }
  .preview-command { max-width: none; justify-content: space-between; }
}
</style>
