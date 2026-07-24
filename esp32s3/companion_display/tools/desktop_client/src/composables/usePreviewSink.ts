import { useStreamStore } from "../stores/stream";

/**
 * 预览渲染入口。Tauri 事件回调把 base64 JPEG 交给 `display`，
 * 停流/出错时调用 `clear`。当前实现走 data URL；未来高帧预览
 * （Blob URL / canvas / 双缓冲）只改本文件内部实现，调用方不变。
 */
export function usePreviewSink() {
  const stream = useStreamStore();

  function display(base64: string) {
    stream.previewSrc = `data:image/jpeg;base64,${base64}`;
    stream.previewReady = true;
  }

  function clear() {
    stream.clearPreview();
  }

  return {
    display,
    clear,
    previewSrc: stream.previewSrc,
    previewReady: stream.previewReady,
  };
}