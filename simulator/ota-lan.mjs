// Simulator of the TMM LAN OTA contract (docs/OTA_LAN_CONTRACT.md).
// Simulation evidence only: it validates message shapes and state
// transitions at the simulated boundary, never electrical behavior.

export const OTA_LAN_SERVICE_TYPE = "_telemetric-ota._tcp";
export const DEVICE_INFO_PATH = "/api/device-info";
export const OTA_IMAGE_PATH = "/api/ota/image";
export const OTA_PORT = 80;
export const APP_IMAGE_OFFSET = 0x10000;
export const OTA_PASSWORD_MIN_LENGTH = 8;

export const DEVICE_INFO_FIELDS = [
  "productCode",
  "deviceId",
  "hardwareRevision",
  "firmwareVersion",
  "chipFamily",
  "flashSize",
  "flashMode",
  "partitionScheme",
  "ip",
  "otaSupported",
  "otaPort",
  "otaPath",
  "mdns"
];

export const DISCOVERY_TXT_FIELDS = [
  "productCode",
  "deviceId",
  "hwRev",
  "fwVer",
  "chipFamily",
  "flashSize",
  "path",
  "otaPath",
  "otaPort"
];

export function buildDiscoveryTxt(identity) {
  return {
    productCode: identity.productCode,
    deviceId: identity.deviceId,
    hwRev: identity.hardwareRevision,
    fwVer: identity.firmwareVersion,
    chipFamily: identity.chipFamily,
    flashSize: identity.flashSize,
    path: DEVICE_INFO_PATH,
    otaPath: OTA_IMAGE_PATH,
    otaPort: String(OTA_PORT)
  };
}

export function buildDeviceInfo(identity, { ip, otaSupported }) {
  return {
    productCode: identity.productCode,
    deviceId: identity.deviceId,
    hardwareRevision: identity.hardwareRevision,
    firmwareVersion: identity.firmwareVersion,
    chipFamily: identity.chipFamily,
    flashSize: identity.flashSize,
    flashMode: identity.flashMode,
    partitionScheme: identity.partitionScheme,
    ip,
    otaSupported,
    otaPort: OTA_PORT,
    otaPath: OTA_IMAGE_PATH,
    mdns: { service: OTA_LAN_SERVICE_TYPE, hostname: identity.hostname }
  };
}

// Canonical releaseId format shared with scripts/build-bringup.ps1:
// TMM-<version without the leading v>-<short commit>, e.g. TMM-0.6.1-0a9113e.
// The `version` field itself keeps the v-prefix (v0.6.1).
export function releaseIdFor(version, buildId) {
  return `TMM-${version.replace(/^v/, "")}-${buildId}`;
}

export function buildArtifactMetadata({ appPath, mergedPath, appBytes, appSha256, mergedBytes, mergedSha256, version, buildId, sourceCommit }) {
  const releaseId = releaseIdFor(version, buildId);
  const base = {
    productCode: "TMM",
    version,
    buildId,
    sourceCommit,
    hardwareRevision: "TMM_V6_R0_M0",
    chipFamily: "ESP32-S3",
    flashSize: "16MB",
    flashMode: "qio",
    partitionScheme: "tmm-ota-4mb"
  };
  return {
    schemaVersion: 2,
    ...base,
    releaseId,
    artifacts: [
      {
        ...base,
        imageType: "app",
        offset: APP_IMAGE_OFFSET,
        fileName: appPath,
        sizeBytes: appBytes,
        sha256: appSha256,
        transport: "lan-ota",
        usbRecovery: false,
        releaseId
      },
      {
        ...base,
        imageType: "full",
        offset: 0,
        fileName: mergedPath,
        sizeBytes: mergedBytes,
        sha256: mergedSha256,
        transport: "usb",
        usbRecovery: true,
        releaseId
      }
    ]
  };
}

const APP_IMAGE_MAGIC = 0xe9;

function constantTimeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string") return false;
  if (a.length !== b.length) return false;
  let difference = 0;
  for (let index = 0; index < a.length; ++index) difference |= a.charCodeAt(index) ^ b.charCodeAt(index);
  return difference === 0;
}

export class TmmOtaLanSimulator {
  constructor({ identity, otaToken, appPartitionBytes, flashSizeBytes }) {
    this.identity = identity;
    this.otaToken = otaToken ?? "";
    this.appPartitionBytes = appPartitionBytes;
    this.flashSizeBytes = flashSizeBytes;
    this.advertised = false;
    this.otaState = "idle";
    this.pendingVerify = false;
    this.stableMs = 0;
  }

  advertise(nowMs = 0) {
    this.advertised = true;
    this._now = nowMs;
    return { service: OTA_LAN_SERVICE_TYPE, txt: buildDiscoveryTxt(this.identity) };
  }

  end() {
    this.advertised = false;
  }

  get(ip, { tokenConfigured = true } = {}) {
    return {
      status: 200,
      body: buildDeviceInfo(this.identity, {
        ip,
        otaSupported: this.advertised && tokenConfigured && this.otaToken.length >= OTA_PASSWORD_MIN_LENGTH
      })
    };
  }

  // Handles POST OTA_IMAGE_PATH. `bytes` is the candidate image body.
  postOtaImage({ token, bytes, nowMs = 0 }) {
    if (!constantTimeEqual(this.otaToken, token)) return { status: 401, body: { ok: false, error: "unauthorized" } };
    if (bytes.length > this.appPartitionBytes) {
      return {
        status: 413,
        body: { ok: false, error: "image_larger_than_ota_slot" }
      };
    }
    if (!bytes.length || bytes[0] !== APP_IMAGE_MAGIC) {
      return { status: 422, body: { ok: false, error: "invalid_app_image" } };
    }
    this.otaState = "restarting";
    this.pendingVerify = true;
    this.stableMs = 0;
    this._now = nowMs;
    return { status: 200, body: { ok: true, state: "restarting" } };
  }

  // After the new slot boots, the device confirms only after
  // STABILITY_CONFIRM_MS of continuously healthy loop iterations.
  confirmStability(healthyMs) {
    if (!this.pendingVerify) return "confirmed";
    this.stableMs += healthyMs;
    if (this.stableMs >= TmmOtaLanSimulator.STABILITY_CONFIRM_MS) {
      this.pendingVerify = false;
      return "confirmed";
    }
    return "pending-verify";
  }

  serviceUnhealthy() {
    if (this.pendingVerify) {
      this.pendingVerify = false;
      this.otaState = "rolled-back";
      return "rolled-back";
    }
    return this.otaState;
  }
}

TmmOtaLanSimulator.STABILITY_CONFIRM_MS = 15000;
