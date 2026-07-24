<script setup lang="ts">
import { computed } from "vue";
import { useStreamStore } from "../stores/stream";
import { useLocaleStore } from "../stores/locale";

const stream = useStreamStore();
const locale = useLocaleStore();

const empty = computed(() => !stream.previewReady);
const limitHint = computed(() => stream.performanceLimited && stream.active);
</script>

<template>
  <div class="stage">
    <div class="frame">
      <img v-if="!empty" :src="stream.previewSrc" :alt="locale.t('preview.alt')" />
      <div v-else class="placeholder">
        <div class="placeholder-title">{{ locale.t("preview.title") }}</div>
        <div class="placeholder-sub">{{ locale.t("preview.subtitle") }}</div>
      </div>
      <div v-if="limitHint" class="limit-badge">{{ locale.t("metric.limited") }}</div>
    </div>
  </div>
</template>

<style scoped>
.stage {
  min-width: 0;
  min-height: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  height: 100%;
}

.frame {
  position: relative;
  width: 100%;
  height: 100%;
  min-height: 260px;
  border: 1px solid var(--cd-border);
  border-radius: var(--cd-radius);
  background: var(--cd-night);
  overflow: hidden;
  display: grid;
  place-items: center;
}

.frame img {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: block;
}

.placeholder {
  color: var(--cd-night-fg);
  text-align: center;
  padding: 0 16px;
}

.placeholder-title {
  font-size: 14px;
  font-weight: 700;
  margin-bottom: 6px;
}

.placeholder-sub {
  font-size: 12px;
}

.limit-badge {
  position: absolute;
  top: 10px;
  right: 10px;
  background: var(--cd-warn-soft);
  color: var(--cd-warn);
  font-size: 12px;
  font-weight: 700;
  border-radius: 999px;
  padding: 4px 12px;
}
</style>
