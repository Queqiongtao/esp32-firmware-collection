<script setup lang="ts">
import { computed, ref } from "vue";
import { NCollapse, NCollapseItem } from "naive-ui";
import { useLogsStore } from "../stores/logs";

const logs = useLogsStore();
const expanded = ref(false);

const entries = computed(() => logs.entries);

function formatTime(t: number): string {
  const d = new Date(t);
  const pad = (n: number) => String(n).padStart(2, "0");
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
</script>

<template>
  <section class="logs">
    <n-collapse
      :default-expanded-names="[]"
      @update:expanded-names="(v: string | string[]) => (expanded = Array.isArray(v) ? v.length > 0 : Boolean(v))"
    >
      <n-collapse-item name="logs">
        <template #header>
          <span class="logs-title">
            Event log
            <span class="logs-count">{{ entries.length }}</span>
          </span>
        </template>
        <div class="logs-body">
          <div v-if="!entries.length" class="logs-empty">No events yet</div>
          <div v-for="entry in entries" :key="entry.id" :class="['log-line', `tone-${entry.tone}`]">
            <span class="log-time">{{ formatTime(entry.time) }}</span>
            <span class="log-text">{{ entry.text }}</span>
          </div>
        </div>
      </n-collapse-item>
    </n-collapse>
  </section>
</template>

<style scoped>
.logs {
  border-top: 1px solid var(--cd-border);
  background: var(--cd-surface);
}

.logs-title {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  font-weight: 700;
}

.logs-count {
  background: var(--cd-neutral-soft);
  color: var(--cd-neutral-ink);
  border-radius: 999px;
  padding: 1px 8px;
  font-size: 11px;
}

.logs-body {
  max-height: 200px;
  overflow-y: auto;
  display: grid;
  gap: 2px;
  font-family: var(--cd-mono);
  font-size: 12px;
}

.logs-empty {
  color: var(--cd-subtle);
  padding: 8px 0;
}

.log-line {
  display: grid;
  grid-template-columns: 64px minmax(0, 1fr);
  gap: 10px;
  padding: 2px 0;
}

.log-time {
  color: var(--cd-subtle);
}

.log-text {
  color: var(--cd-strong);
  overflow-wrap: anywhere;
}

.tone-good .log-text {
  color: var(--cd-good);
}
.tone-warn .log-text {
  color: var(--cd-warn);
}
.tone-bad .log-text {
  color: var(--cd-bad);
}
</style>