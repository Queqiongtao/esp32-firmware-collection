import { defineStore } from "pinia";
import { ref } from "vue";
import type { TranslationKey, TranslationParams } from "../i18n/messages";
import type { LogTone } from "../types/tauri";

const MAX_LOGS = 500;

export type LogEntry = {
  id: number;
  time: number;
  tone: LogTone;
  text?: string;
  key?: TranslationKey;
  params?: TranslationParams;
};

export const useLogsStore = defineStore("logs", () => {
  const entries = ref<LogEntry[]>([]);
  const nextId = ref(1);

  function append(entry: Omit<LogEntry, "id" | "time">) {
    entries.value.push({ id: nextId.value++, time: Date.now(), ...entry });
    if (entries.value.length > MAX_LOGS) entries.value.splice(0, entries.value.length - MAX_LOGS);
  }

  function pushRaw(text: string, tone: LogTone = "normal") {
    const trimmed = text.trim();
    if (trimmed) append({ tone, text: trimmed });
  }

  function pushKey(key: TranslationKey, tone: LogTone = "normal", params: TranslationParams = {}, rawDetail = "") {
    append({ tone, key, params, text: rawDetail.trim() || undefined });
  }

  function clear() {
    entries.value = [];
  }

  return { entries, pushRaw, pushKey, clear };
});
