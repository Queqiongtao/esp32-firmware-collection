<script setup lang="ts">
import { onMounted } from "vue";
import { NConfigProvider, NMessageProvider, NDialogProvider, useMessage } from "naive-ui";
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
import { useNaiveTheme } from "./theme/naivetheme";
import { useTauriEvents } from "./composables/useTauriEvents";
import { useStreamLifecycle } from "./composables/useStreamLifecycle";

const theme = useThemeStore();
const settings = useSettingsStore();
const device = useDeviceStore();
const stream = useStreamStore();
const logs = useLogsStore();
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
          <MetricBar />
          <div class="workspace">
            <PreviewStage />
            <SidePanels
              :sourceId="settings.sourceId"
              :sources="lc.sources.value"
              :sourceLabel="lc.sourceLabel"
              :locked="lc.locked.value"
              :readyLabel="lc.readyLabel.value"
              :readyTone="lc.readyTone.value"
              :startDisabled="lc.startDisabled.value"
              :stopDisabled="lc.stopDisabled.value"
              @update:sourceId="(v: string) => (settings.sourceId = v)"
              @refresh="lc.refreshSources"
              @check="lc.checkDeviceNow"
              @update:videoTransport="onTransportChange"
              @input-params="lc.scheduleParamsUpdate"
              @input-endpoint="lc.scheduleProbe"
              @input-token="lc.scheduleProbe"
              @start="lc.start"
              @stop="lc.stop"
            />
          </div>
          <LogPanel />
        </div>
      </n-dialog-provider>
    </n-message-provider>
  </n-config-provider>
</template>

<style scoped>
.app {
  width: min(1480px, 100%);
  margin: 0 auto;
  padding: 20px 22px;
  display: flex;
  flex-direction: column;
  min-height: 100vh;
  gap: 14px;
}

.workspace {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 340px;
  gap: 16px;
  min-height: 0;
  flex: 1 1 auto;
}

@media (max-width: 1100px) {
  .workspace {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
