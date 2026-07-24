export type CaptureSource = {
  id: string;
  kind: string;
  label: string;
  width: number;
  height: number;
};

export type StreamEvent = {
  state: string;
  detail: string;
  fps: number;
  jpegBytes: number;
  droppedFrames: number;
  videoTransport: string;
  captureEncodeMs: number;
  captureWaitMs: number;
  deviceMs: number;
  deviceReplacedFrames: number;
};

export type UdpDiagnostics = {
  rxPackets: number;
  acceptedFragments: number;
  rejectedPackets: number;
  completedFrames: number;
  submittedFrames: number;
  submitFailures: number;
  reassemblyTimeouts: number;
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
};

export type LogTone = "normal" | "good" | "warn" | "bad";

export type LogEntry = {
  id: number;
  time: number;
  tone: LogTone;
  text: string;
};