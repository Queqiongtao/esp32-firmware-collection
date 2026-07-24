import type { GlobalTheme } from "naive-ui";
import { darkTheme } from "naive-ui";
import { computed } from "vue";
import { useThemeStore } from "../stores/theme";

export function useNaiveTheme() {
  const theme = useThemeStore();
  const naive = computed<GlobalTheme | null>(() => (theme.dark ? darkTheme : null));

  const overrides = computed(() =>
    theme.dark
      ? {
          common: {
            primaryColor: "#2bb6c8",
            primaryColorHover: "#3fc6d8",
            primaryColorPressed: "#22a3b3",
            primaryColorSuppl: "#2bb6c8",
            borderRadius: "8px",
            borderRadiusSmall: "6px",
          },
        }
      : {
          common: {
            primaryColor: "#127f8e",
            primaryColorHover: "#1692a3",
            primaryColorPressed: "#0f6d7a",
            primaryColorSuppl: "#127f8e",
            borderRadius: "8px",
            borderRadiusSmall: "6px",
          },
        },
  );

  return { naive, overrides };
}