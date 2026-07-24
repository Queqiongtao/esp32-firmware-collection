import { defineStore } from "pinia";
import { computed, ref, watch } from "vue";
import type { DisplayOrientation } from "../types/tauri";

const STORAGE_KEY = "companion-display.settings.v1";

export const useSettingsStore = defineStore("settings", () => {
  const endpoint = ref("");
  const token = ref("");
  const sourceId = ref("");
  const fps = ref(25);
  const quality = ref(70);
  const videoTransport = ref("tcp");
  const displayOrientation = ref<DisplayOrientation>("portrait");
  const cropEnabled = ref(false);
  const regionX = ref(0);
  const regionY = ref(0);
  const regionWidth = ref(0);
  const regionHeight = ref(0);

  const normalizedEndpoint = computed(() => {
    const trimmed = endpoint.value.trim();
    if (!trimmed) return "";
    const withScheme = trimmed.startsWith("ws://") ? trimmed : `ws://${trimmed}`;
    const withoutTrailingSlash = withScheme.endsWith("/") ? withScheme.slice(0, -1) : withScheme;
    return withoutTrailingSlash.includes("/stream") ? withoutTrailingSlash : `${withoutTrailingSlash}/stream`;
  });

  function normalizeEndpointInPlace() {
    endpoint.value = normalizedEndpoint.value;
  }

  function sanitizeToken() {
    token.value = token.value.replace(/\D/g, "").slice(0, 6);
  }

  function save() {
    try {
      localStorage.setItem(
        STORAGE_KEY,
        JSON.stringify({
          endpoint: endpoint.value,
          token: token.value,
          fps: fps.value,
          quality: quality.value,
          videoTransport: videoTransport.value,
          displayOrientation: displayOrientation.value,
          cropEnabled: cropEnabled.value,
          regionX: regionX.value,
          regionY: regionY.value,
          regionWidth: regionWidth.value,
          regionHeight: regionHeight.value,
        }),
      );
    } catch {
      /* ignore */
    }
  }

  function restore() {
    let raw: string | null = null;
    try {
      raw = localStorage.getItem(STORAGE_KEY);
    } catch {
      return;
    }
    if (!raw) return;
    try {
      const saved = JSON.parse(raw) as Record<string, unknown>;
      if (typeof saved.endpoint === "string") endpoint.value = saved.endpoint;
      if (typeof saved.token === "string") token.value = saved.token;
      if (typeof saved.fps === "number") fps.value = saved.fps;
      else if (typeof saved.fps === "string") fps.value = Number(saved.fps);
      if (typeof saved.quality === "number") quality.value = saved.quality;
      else if (typeof saved.quality === "string") quality.value = Number(saved.quality);
      if (typeof saved.videoTransport === "string") videoTransport.value = saved.videoTransport;
      if (
        saved.displayOrientation === "portrait" ||
        saved.displayOrientation === "landscape_left" ||
        saved.displayOrientation === "portrait_inverted" ||
        saved.displayOrientation === "landscape_right"
      ) {
        displayOrientation.value = saved.displayOrientation;
      }
      if (typeof saved.cropEnabled === "boolean") cropEnabled.value = saved.cropEnabled;
      if (typeof saved.regionX === "number") regionX.value = saved.regionX;
      else if (typeof saved.regionX === "string") regionX.value = Number(saved.regionX);
      if (typeof saved.regionY === "number") regionY.value = saved.regionY;
      else if (typeof saved.regionY === "string") regionY.value = Number(saved.regionY);
      if (typeof saved.regionWidth === "number") regionWidth.value = saved.regionWidth;
      else if (typeof saved.regionWidth === "string") regionWidth.value = Number(saved.regionWidth);
      if (typeof saved.regionHeight === "number") regionHeight.value = saved.regionHeight;
      else if (typeof saved.regionHeight === "string") regionHeight.value = Number(saved.regionHeight);
    } catch {
      /* ignore */
    }
    sanitizeToken();
    normalizeEndpointInPlace();
  }

  watch(
    [endpoint, token, fps, quality, videoTransport, displayOrientation, cropEnabled, regionX, regionY, regionWidth, regionHeight],
    () => save(),
  );

  return {
    endpoint,
    token,
    sourceId,
    fps,
    quality,
    videoTransport,
    displayOrientation,
    cropEnabled,
    regionX,
    regionY,
    regionWidth,
    regionHeight,
    normalizedEndpoint,
    normalizeEndpointInPlace,
    sanitizeToken,
    save,
    restore,
  };
});
