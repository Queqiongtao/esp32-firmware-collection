import { computed, ref, watch } from "vue";
import { defineStore } from "pinia";
import { enUS, zhCN, type TranslationKey, type TranslationParams } from "../i18n/messages";

export type AppLocale = "zh-CN" | "en-US";

const STORAGE_KEY = "companion-display.locale.v1";

function initialLocale(): AppLocale {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved === "zh-CN" || saved === "en-US") return saved;
  } catch {
    /* ignore */
  }
  const languages = navigator.languages?.length ? navigator.languages : [navigator.language];
  return languages.some((language) => language.toLowerCase().startsWith("zh")) ? "zh-CN" : "en-US";
}

export const useLocaleStore = defineStore("locale", () => {
  const locale = ref<AppLocale>(initialLocale());
  const messages = computed(() => (locale.value === "zh-CN" ? zhCN : enUS));

  function t(key: TranslationKey, params: TranslationParams = {}): string {
    return messages.value[key].replace(/\{(\w+)\}/g, (_, name: string) => String(params[name] ?? `{${name}}`));
  }

  function setLocale(next: AppLocale) {
    locale.value = next;
  }

  function toggle() {
    setLocale(locale.value === "zh-CN" ? "en-US" : "zh-CN");
  }

  function apply() {
    document.documentElement.lang = locale.value;
  }

  watch(
    locale,
    (next) => {
      try {
        localStorage.setItem(STORAGE_KEY, next);
      } catch {
        /* ignore */
      }
      apply();
    },
    { immediate: true },
  );

  return { locale, t, setLocale, toggle, apply };
});

const STREAM_STATE_KEYS: Record<string, TranslationKey> = {
  idle: "status.idle",
  error: "status.error",
  dropping: "status.dropping",
  stopped: "status.stopped",
  stopping: "status.stopping",
  streaming: "status.streaming",
  connecting: "status.connecting",
  negotiating: "status.negotiating",
  reconnecting: "status.reconnecting",
  "requesting screen share": "status.requestingScreenShare",
  "waiting for pipewire frames": "status.waitingPipeWire",
  "waiting first screenshot": "status.waitingFirstFrame",
  "snapshot sent": "status.snapshotSent",
  capturing: "status.capturing",
  "frame ready": "status.frameReady",
};

export function streamStateKey(state: string): TranslationKey | null {
  return STREAM_STATE_KEYS[state.trim().toLowerCase()] ?? null;
}
