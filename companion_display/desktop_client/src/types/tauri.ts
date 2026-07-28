export type CaptureSource = {
  id: string;
  kind: string;
  label: string;
  width: number;
  height: number;
};

export type DisplayOrientation =
  | "portrait"
  | "landscape_left"
  | "portrait_inverted"
  | "landscape_right";
export type ProjectionMode = "live" | "region";
export type CaptureRegion = { x: number; y: number; width: number; height: number };
export type RegionSelectionPreview = {
  id: string;
  width: number;
  height: number;
  contentX: number;
  contentY: number;
  contentWidth: number;
  contentHeight: number;
  sourceWidth: number;
  sourceHeight: number;
  jpegBase64: string;
};

export type StreamEvent = {
  state: string;
  detail: string;
  fps: number;
  jpegBytes: number;
  jpegQualityUsed: number;
  sourceScalePercent: number;
  droppedFrames: number;
  videoTransport: string;
  captureEncodeMs: number;
  captureWaitMs: number;
  captureProcessMs: number;
  sendMs: number;
  jpegEncodeAttempts: number;
  deviceMs: number;
  deviceReplacedFrames: number;
  udpFragmentBudget: number;
  udpFecRecoveredFragments: number;
  udpUnrecoverableFrames: number;
  projectionMode: ProjectionMode;
};

export type StreamTarget = {
  width: number;
  height: number;
  maxFrameBytes: number;
};

export type UdpDiagnostics = {
  rxPackets: number;
  acceptedFragments: number;
  rejectedPackets: number;
  completedFrames: number;
  submittedFrames: number;
  submitFailures: number;
  reassemblyTimeouts: number;
  fecPackets: number;
  fecRecoveredFragments: number;
  fecUnrecoverableFrames: number;
  lastReject: string;
};

export type Performance = {
  sequence: number;
  queueWaitUs: number;
  jpegDecodeUs: number;
  lcdWaitUs: number;
  lcdSubmitUs: number;
  previousDmaUs: number;
  replacedFrames: number;
};

export type DeviceStatus = {
  name: string;
  width: number;
  height: number;
  wifi: string;
  ipv4: string;
  activeTransport: string;
  tokenSet: boolean;
  protocolVersion: number;
  videoTransports: string[];
  touch: boolean;
  touchEventsActive: boolean;
  droppedFrames: number;
  performance: Performance;
  udp: UdpDiagnostics;
};

export type StreamConfig = {
  endpoint: string;
  token: string;
  sourceId: string;
  regionX: number;
  regionY: number;
  regionWidth: number;
  regionHeight: number;
  fps: number;
  jpegQuality: number;
  videoTransport: string;
  displayOrientation: DisplayOrientation;
  portalRestoreToken?: string;
  projectionMode: ProjectionMode;
  selectionId?: string;
  touchControl: boolean;
};

export type LogTone = "normal" | "good" | "warn" | "bad";
