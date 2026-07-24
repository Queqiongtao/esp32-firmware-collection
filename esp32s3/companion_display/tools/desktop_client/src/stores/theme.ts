import { defineStore } from "pinia";
import { ref, watch } from "vue";

export type ThemeMode = "light" | "dark" | "auto";

const STORAGE_KEY = "companion-display.theme.v1";

function readInitial(): ThemeMode {
  try {
    const v = localStorage.getItem(STORAGE_KEY);
    if (v === "light" || v === "dark" || v === "auto") return v;
  } catch {
    /* ignore */
  }
  return "auto";
}

function resolveDark(mode: ThemeMode): boolean {
  if (mode === "dark") return true;
  if (mode === "light") return false;
  return window.matchMedia?.("(prefers-color-scheme: dark)").matches ?? false;
}

export const useThemeStore = defineStore("theme", () => {
  const mode = ref<ThemeMode>(readInitial());
  const dark = ref<boolean>(resolveDark(mode.value));

  function apply() {
    dark.value = resolveDark(mode.value);
    document.documentElement.dataset.theme = dark.value ? "dark" : "light";
  }

  function setMode(next: ThemeMode) {
    mode.value = next;
  }

  watch(mode, (next) => {
    try {
      localStorage.setItem(STORAGE_KEY, next);
    } catch {
      /* ignore */
    }
    apply();
  });

  let mediaCleanup: (() => void) | null = null;
  function bindMedia() {
    const mq = window.matchMedia?.("(prefers-color-scheme: dark)");
    if (!mq) return;
    const handler = () => apply();
    mq.addEventListener("change", handler);
    mediaCleanup = () => mq.removeEventListener("change", handler);
    if (import.meta.hot) {
      import.meta.hot.dispose(() => mediaCleanup?.());
    }
  }

  return { mode, dark, setMode, apply, bindMedia };
});