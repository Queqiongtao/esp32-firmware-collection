import { useStreamStore } from "../stores/stream";
import { useSettingsStore } from "../stores/settings";
import type { DisplayOrientation } from "../types/tauri";

/**
 * 预览渲染入口。Tauri 事件回调把 base64 JPEG 交给 `display`，
 * 停流/出错时调用 `clear`。当前实现走 data URL；未来高帧预览
 * （Blob URL / canvas / 双缓冲）只改本文件内部实现，调用方不变。
 */
export function usePreviewSink() {
  const stream = useStreamStore();
  const settings = useSettingsStore();
  let generation = 0;

  function display(base64: string) {
    const currentGeneration = ++generation;
    const orientation = settings.displayOrientation;
    const source = `data:image/jpeg;base64,${base64}`;
    if (orientation === "portrait") {
      stream.previewSrc = source;
      stream.previewReady = true;
      return;
    }
    const image = new Image();
    image.onload = () => {
      if (currentGeneration !== generation) return;
      const landscape = orientation === "landscape_left" || orientation === "landscape_right";
      const canvas = document.createElement("canvas");
      canvas.width = landscape ? image.height : image.width;
      canvas.height = landscape ? image.width : image.height;
      const context = canvas.getContext("2d");
      if (!context) return;
      applyPreviewTransform(context, orientation, image.width, image.height);
      context.drawImage(image, 0, 0);
      if (currentGeneration !== generation) return;
      stream.previewSrc = canvas.toDataURL("image/jpeg", 0.9);
      stream.previewReady = true;
    };
    image.src = source;
  }

  function clear() {
    generation++;
    stream.clearPreview();
  }

  return {
    display,
    clear,
    previewSrc: stream.previewSrc,
    previewReady: stream.previewReady,
  };
}

function applyPreviewTransform(
  context: CanvasRenderingContext2D,
  orientation: DisplayOrientation,
  width: number,
  height: number,
) {
  switch (orientation) {
    case "landscape_left":
      context.translate(0, width);
      context.rotate(-Math.PI / 2);
      break;
    case "portrait_inverted":
      context.translate(width, height);
      context.rotate(Math.PI);
      break;
    case "landscape_right":
      context.translate(height, 0);
      context.rotate(Math.PI / 2);
      break;
    case "portrait":
      break;
  }
}
