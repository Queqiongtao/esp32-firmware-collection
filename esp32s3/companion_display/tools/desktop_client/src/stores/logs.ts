import { defineStore } from "pinia";
import { ref } from "vue";
import type { LogEntry, LogTone } from "../types/tauri";

const MAX_LOGS = 500;

export const useLogsStore = defineStore("logs", () => {
  const entries = ref<LogEntry[]>([]);
  const nextId = ref(1);

  function push(text: string, tone: LogTone = "normal") {
    const trimmed = text.trim();
    if (!trimmed) return;
    entries.value.push({ id: nextId.value++, time: Date.now(), tone, text: trimmed });
    if (entries.value.length > MAX_LOGS) {
      entries.value.splice(0, entries.value.length - MAX_LOGS);
    }
  }

  function clear() {
    entries.value = [];
  }

  return { entries, push, clear };
});