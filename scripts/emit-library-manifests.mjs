// Emits the two firmware-library manifests for one TMM release package from
// the build's artifacts.json: manifest-full.json (imageType full, transport
// usb) and manifest-app-only.json (imageType app-only, transport ota). Both
// carry identical shared fields (PACKAGE_SHARED_FIELDS contract) and their
// own physical SHA-256 + byte size, re-verified against the BIN files, so
// the handoff to the firmware library publisher is a straight copy.
//
// Usage:
//   node scripts/emit-library-manifests.mjs --artifacts <artifacts.json> \
//     --out-dir <release-dir> --source-repository <url>
//
// The manifests are handoff metadata for the library publisher; they are not
// committed and never contain credentials or passwords.

import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";

function argument(name) {
  const index = process.argv.indexOf(name);
  return index >= 0 ? process.argv[index + 1] : undefined;
}

const artifactsPath = argument("--artifacts");
const outDir = argument("--out-dir");
const sourceRepository = argument("--source-repository");
if (!artifactsPath || !outDir || !sourceRepository) {
  console.error("usage: node scripts/emit-library-manifests.mjs --artifacts <artifacts.json> --out-dir <dir> --source-repository <url>");
  process.exit(1);
}

const build = JSON.parse(readFileSync(artifactsPath, "utf8").replace(/^\uFEFF/, ""));
const app = build.artifacts.find((artifact) => artifact.imageType === "app");
const full = build.artifacts.find((artifact) => artifact.imageType === "full");
if (!app || !full) {
  console.error("artifacts.json must record one 'app' and one 'full' artifact");
  process.exit(1);
}

// Physical files are re-hashed; any drift from artifacts.json fails here.
function physical(artifact) {
  const bytes = readFileSync(join(dirname(artifactsPath), artifact.fileName));
  const sha256 = createHash("sha256").update(bytes).digest("hex");
  if (sha256 !== artifact.sha256 || bytes.length !== artifact.sizeBytes) {
    console.error(`physical file does not match artifacts.json: ${artifact.fileName}`);
    process.exit(1);
  }
  return { fileName: artifact.fileName, sizeBytes: bytes.length, sha256 };
}

const appPhysical = physical(app);
const fullPhysical = physical(full);

// Canonical releaseId: TMM-<version without v>-<short commit>. The build's
// releaseId already follows it; the buildId field here is the plain short
// commit (the build script's "-lan-ota" suffix is internal metadata).
const version = String(build.version).replace(/^v/, "");
const expectedReleaseId = `${build.productCode}-${version}-${build.sourceCommit.slice(0, 7)}`;
if (build.releaseId !== expectedReleaseId) {
  console.error(`releaseId ${build.releaseId} is not canonical (${expectedReleaseId})`);
  process.exit(1);
}

const releaseNotes =
  `TMM v${version} Hardware QC release (firmwareRole qc, stage lab): keeps every proven ` +
  "QC capability of v0.6.0/v0.6.1 - the six mandatory items (LED2-LED10 active-low sequence+manual, " +
  "BOOT/CHANGE DISPLAY button transitions, AHT10 datasheet-ordered sampling, W5500 link+DHCP, SD " +
  "write/readback/cleanup with card-presence probe, RS485 Modbus RTU master with 3-valid-streak PASS), " +
  "the guided operator portal with clickable tutorial, and portal discovery + authenticated app-only " +
  "OTA (D-023/D-024/D-025). Adds a machine-readable qcSummary roll-up to /api/status (D-027) that " +
  "distinguishes untested/testing/pass/fail/manual per item; LED and buttons stay manual-only and " +
  "nothing converts compile success or live evidence into an automatic hardware PASS. Partition table " +
  "tmm-ota-4mb is byte-identical to the board-proven layout (D-024/D-026): app-only OTA from a v0.6.x " +
  "device stays compatible; USB merged flash remains the first-install/recovery path. OTA slot limit: " +
  "0x1F0000 (2,031,616 bytes) per slot. Compile evidence only - physical flash/provision/QC/OTA on " +
  "hardware is still pending.";

function libraryManifest(imageType, offset, facts) {
  return {
    schemaVersion: 1,
    productCode: build.productCode,
    productName: "Telemetric Module Master",
    version,
    buildId: build.sourceCommit.slice(0, 7),
    releaseId: build.releaseId,
    releaseTag: `${build.productCode}-v${version}`,
    sourceRepository,
    sourceCommit: build.sourceCommit,
    channel: "development",
    lifecycle: "draft",
    evidenceLevel: "built",
    stage: "lab",
    firmwareRole: "qc",
    hardwareRevision: build.hardwareRevision,
    chipFamily: build.chipFamily,
    flashSize: build.flashSize,
    flashMode: build.flashMode,
    partitionScheme: build.partitionScheme,
    imageType,
    offset,
    erasePolicy: "prompt",
    wifiCapable: true,
    fileName: facts.fileName,
    sizeBytes: facts.sizeBytes,
    sha256: facts.sha256,
    releaseNotes,
    createdAt: new Date().toISOString()
  };
}

const fullManifest = libraryManifest("full", 0, fullPhysical);
const appManifest = libraryManifest("app-only", app.offset, appPhysical);

writeFileSync(join(outDir, "manifest-full.json"), JSON.stringify(fullManifest, null, 2) + "\n");
writeFileSync(join(outDir, "manifest-app-only.json"), JSON.stringify(appManifest, null, 2) + "\n");

console.log(JSON.stringify({
  releaseId: build.releaseId,
  version: build.version,
  firmwareRole: "qc",
  stage: "lab",
  manifestFull: fullManifest.fileName,
  manifestAppOnly: appManifest.fileName
}, null, 2));