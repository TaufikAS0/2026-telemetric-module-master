import test from "node:test";
import assert from "node:assert/strict";
import {
  OTA_LAN_SERVICE_TYPE,
  DEVICE_INFO_PATH,
  OTA_IMAGE_PATH,
  APP_IMAGE_OFFSET,
  DEVICE_INFO_FIELDS,
  DISCOVERY_TXT_FIELDS,
  buildDiscoveryTxt,
  buildDeviceInfo,
  buildArtifactMetadata,
  TmmOtaLanSimulator
} from "../simulator/ota-lan.mjs";

const identity = {
  productCode: "TMM",
  deviceId: "aabbccddeeff",
  hardwareRevision: "TMM_V6_R0_M0",
  firmwareVersion: "v0.6.0",
  chipFamily: "ESP32-S3",
  flashSize: "4MB",
  flashMode: "dio",
  partitionScheme: "tmm-ota-4mb",
  hostname: "tmm-v6-r0-m0"
};

function validApp() {
  const bytes = new Uint8Array(2048);
  bytes[0] = 0xe9;
  return bytes;
}

test("device identity contract carries the TMM identity", () => {
  const info = buildDeviceInfo(identity, { ip: "192.168.1.50", otaSupported: true });
  for (const field of DEVICE_INFO_FIELDS) assert.ok(field in info, field);
  assert.equal(info.productCode, "TMM");
  assert.equal(info.deviceId, "aabbccddeeff");
  assert.equal(info.hardwareRevision, "TMM_V6_R0_M0");
  assert.equal(info.chipFamily, "ESP32-S3");
  assert.equal(info.flashSize, "4MB");
  assert.equal(info.otaSupported, true);
  assert.equal(info.otaPort, 80);
  assert.equal(info.otaPath, OTA_IMAGE_PATH);
  assert.equal(info.mdns.service, OTA_LAN_SERVICE_TYPE);
  assert.equal(DEVICE_INFO_PATH, "/api/device-info");
});

test("discovery advertisement exposes the required TXT keys", () => {
  const discovery = buildDiscoveryTxt(identity);
  assert.equal(OTA_LAN_SERVICE_TYPE, "_telemetric-ota._tcp");
  for (const key of DISCOVERY_TXT_FIELDS) assert.ok(key in discovery, key);
  assert.equal(discovery.hwRev, "TMM_V6_R0_M0");
  assert.equal(discovery.path, "/api/device-info");
});

test("device-info reports otaSupported false without a provisioned token", () => {
  const device = new TmmOtaLanSimulator({ identity, otaToken: "", appPartitionBytes: 0x1f0000 });
  device.advertise();
  const response = device.get("192.168.1.50");
  assert.equal(response.status, 200);
  assert.equal(response.body.otaSupported, false);
});

test("OTA image endpoint rejects missing and wrong tokens", () => {
  const device = new TmmOtaLanSimulator({ identity, otaToken: "correct-horse", appPartitionBytes: 0x1f0000 });
  assert.equal(device.postOtaImage({ token: undefined, bytes: validApp() }).status, 401);
  assert.equal(device.postOtaImage({ token: "wrong-horse-1", bytes: validApp() }).status, 401);
  assert.equal(device.postOtaImage({ token: "correct-horse", bytes: validApp() }).status, 200);
});

test("OTA image endpoint rejects a full merged image that cannot fit an OTA slot", () => {
  const device = new TmmOtaLanSimulator({ identity, otaToken: "correct-horse", appPartitionBytes: 0x1f0000 });
  const merged = new Uint8Array(4194304); // full 4 MB image incl. bootloader + partition table
  merged[0] = 0xe9;
  const response = device.postOtaImage({ token: "correct-horse", bytes: merged });
  assert.equal(response.status, 413);
  assert.equal(response.body.error, "image_larger_than_ota_slot");
});

test("OTA image endpoint accepts an app-only image and reports restarting", () => {
  const device = new TmmOtaLanSimulator({ identity, otaToken: "correct-horse", appPartitionBytes: 0x1f0000 });
  const response = device.postOtaImage({ token: "correct-horse", bytes: validApp() });
  assert.equal(response.status, 200);
  assert.equal(response.body.state, "restarting");
  assert.equal(device.pendingVerify, true);
});

test("rollback confirms only after sustained healthy loop time", () => {
  const device = new TmmOtaLanSimulator({ identity, otaToken: "correct-horse", appPartitionBytes: 0x1f0000 });
  device.postOtaImage({ token: "correct-horse", bytes: validApp() });
  assert.equal(device.confirmStability(5000), "pending-verify");
  assert.equal(device.confirmStability(5000), "pending-verify");
  assert.equal(device.confirmStability(4999), "pending-verify");
  assert.equal(device.confirmStability(1), "confirmed");
  assert.equal(device.confirmStability(1), "confirmed");
});

test("rollback restores the previous slot when the new app fails before confirmation", () => {
  const device = new TmmOtaLanSimulator({ identity, otaToken: "correct-horse", appPartitionBytes: 0x1f0000 });
  device.postOtaImage({ token: "correct-horse", bytes: validApp() });
  assert.equal(device.serviceUnhealthy(), "rolled-back");
  assert.equal(device.pendingVerify, false);
});

test("artifact metadata distinguishes the OTA app image from the USB merged image", () => {
  const metadata = buildArtifactMetadata({
    version: "0.7.0",
    buildId: "test-build",
    sourceCommit: "7161baaaa72941d182e68ed124c8de54788381ae",
    appPath: "tmm_v6_r0_m0.ino.bin",
    mergedPath: "tmm_v6_r0_m0.ino.merged.bin",
    appBytes: 1835008,
    appSha256: "a".repeat(64),
    mergedBytes: 4194304,
    mergedSha256: "b".repeat(64)
  });
  assert.equal(metadata.artifacts.length, 2);
  const [app, merged] = metadata.artifacts;
  assert.equal(app.imageType, "app");
  assert.equal(app.transport, "lan-ota");
  assert.equal(app.offset, APP_IMAGE_OFFSET);
  assert.equal(app.usbRecovery, false);
  assert.equal(merged.imageType, "full");
  assert.equal(merged.transport, "usb");
  assert.equal(merged.offset, 0);
  assert.equal(merged.usbRecovery, true);
  assert.notEqual(app.sha256, merged.sha256);
  assert.notEqual(app.sizeBytes, merged.sizeBytes);
});
