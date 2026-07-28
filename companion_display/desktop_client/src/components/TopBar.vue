<script setup lang="ts">
import { computed } from "vue";
import { NButton } from "naive-ui";
import { useDeviceStore } from "../stores/device";
import { useStreamStore } from "../stores/stream";
import { useThemeStore } from "../stores/theme";
import type { ThemeMode } from "../stores/theme";
import { useLocaleStore } from "../stores/locale";

const device = useDeviceStore();
const stream = useStreamStore();
const theme = useThemeStore();
const locale = useLocaleStore();

const toneClass = (tone: string) => `tone-${tone}`;

const translatedThemeLabel = computed(() => locale.t(`theme.${theme.mode}` as "theme.auto" | "theme.light" | "theme.dark"));
function cycleTheme() {
  const order: ThemeMode[] = ["auto", "light", "dark"];
  const idx = order.indexOf(theme.mode);
  theme.setMode(order[(idx + 1) % order.length]);
}
</script>

<template>
  <header class="topbar">
    <div class="brand">
      <h1>Companion Display</h1>
      <p class="subtitle">{{ locale.t("app.subtitle") }}</p>
    </div>
    <div class="topbar-right">
      <span :class="['pill', toneClass(device.devicePillTone)]">{{ device.devicePillText }}</span>
      <span :class="['pill', toneClass(stream.pillTone)]">{{ stream.pillText }}</span>
      <n-button quaternary size="small" :title="locale.t('language.switch')" @click="locale.toggle">
        {{ locale.t("language.current") }}
      </n-button>
      <n-button quaternary size="small" class="theme-btn" :title="locale.t('theme.title', { mode: translatedThemeLabel })" @click="cycleTheme">
        <span class="theme-icon">{{ theme.dark ? "☾" : "☼" }}</span>
        <span class="theme-text">{{ translatedThemeLabel }}</span>
      </n-button>
    </div>
  </header>
</template>

<style scoped>
.topbar {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 18px;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--cd-border);
}

.brand h1 {
  margin: 0;
  font-size: 22px;
  line-height: 1.15;
  color: var(--cd-strong);
}

.subtitle {
  margin: 6px 0 0;
  color: var(--cd-muted);
  font-size: 13px;
}

.topbar-right {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
  justify-content: flex-end;
}

.pill {
  display: inline-flex;
  align-items: center;
  min-height: 26px;
  border-radius: 999px;
  padding: 4px 10px;
  background: var(--cd-neutral-soft);
  color: var(--cd-neutral-ink);
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
.tone-bad {
  background: var(--cd-bad-soft);
  color: var(--cd-bad);
}
.tone-active {
  background: var(--cd-accent-soft);
  color: var(--cd-accent-ink);
}

.theme-btn {
  --n-margin: 0 0 0 4px;
}

.theme-icon {
  font-size: 14px;
  margin-right: 4px;
}

@media (max-width: 720px) {
  .topbar { align-items: center; }
  .subtitle { display: none; }
  .brand h1 { font-size: 19px; }
  .topbar-right { gap: 4px; }
  .theme-text { display: none; }
  .theme-icon { margin-right: 0; }
}

@media (max-width: 520px) {
  .topbar { align-items: flex-start; flex-direction: column; }
  .topbar-right { width: 100%; justify-content: flex-start; }
}
</style>
