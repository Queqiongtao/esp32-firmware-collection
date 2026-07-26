import { defineStore } from "pinia";
import { computed, ref, watch } from "vue";
import type { DisplayOrientation, ProjectionMode } from "../types/tauri";

const STORAGE_KEY = "companion-display.settings.v1";

export const useSettingsStore = defineStore("settings", () => {
  const endpoint = ref("");
  const token = ref("");
  const sourceId = ref("");
  const projectionMode = ref<ProjectionMode>("live");
  const fps = ref(25);
  const quality = ref(70);
  const videoTransport = ref("tcp");
  const displayOrientation = ref<DisplayOrientation>("portrait");
  const portalRestoreToken = ref("");
  const touchControl = ref(false);

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
          projectionMode: projectionMode.value,
          fps: fps.value,
          quality: quality.value,
          videoTransport: videoTransport.value,
          displayOrientation: displayOrientation.value,
          portalRestoreToken: portalRestoreToken.value,
          touchControl: touchControl.value,
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
      if (saved.projectionMode === "live" || saved.projectionMode === "region") projectionMode.value = saved.projectionMode;
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
      if (typeof saved.portalRestoreToken === "string") portalRestoreToken.value = saved.portalRestoreToken;
      if (typeof saved.touchControl === "boolean") touchControl.value = saved.touchControl;
    } catch {
      /* ignore */
    }
    sanitizeToken();
    normalizeEndpointInPlace();
  }

  watch(
    [endpoint, token, projectionMode, fps, quality, videoTransport, displayOrientation, portalRestoreToken, touchControl],
    () => save(),
  );

  /* On Linux the touch switch changes the portal session type (ScreenCast vs
  RemoteDesktop); their restore tokens are not interchangeable. */
  watch(touchControl, () => {
    portalRestoreToken.value = "";
  });

  return {
    endpoint,
    token,
    sourceId,
    projectionMode,
    fps,
    quality,
    videoTransport,
    displayOrientation,
    portalRestoreToken,
    touchControl,
    normalizedEndpoint,
    normalizeEndpointInPlace,
    sanitizeToken,
    clearPortalRestoreToken: () => (portalRestoreToken.value = ""),
    save,
    restore,
  };
});
