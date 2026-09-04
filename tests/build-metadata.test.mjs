import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { buildArtifactMetadata, releaseIdFor } from "../simulator/ota-lan.mjs";

const scriptUrl = new URL("../scripts/build-bringup.ps1", import.meta.url);
const versionHeaderUrl = new URL("../firmware/bringup/tmm_v6_r0_m0/tmm_v6_r0_m0_version.h", import.meta.url);

test("metadata builder produces the canonical releaseId used by the PowerShell build", async () => {
  // Read the real embedded version header, exactly as build-bringup.ps1 does.
  const header = await readFile(versionHeaderUrl, "utf8");
  const major = header.match(/#define\s+TMM_M0_VERSION_MAJOR\s+(\d+)/)?.[1];
  const minor = header.match(/#define\s+TMM_M0_VERSION_MINOR\s+(\d+)/)?.[1];
  const patch = header.match(/#define\s+TMM_M0_VERSION_PATCH\s+(\d+)/)?.[1];
  assert.ok(major && minor && patch, "version header defines the three macros");
  const version = `v${major}.${minor}.${patch}`;

  // Same inputs the PowerShell build feeds its releaseId line.
  const buildId = "0a9113e";
  const metadata = buildArtifactMetadata({
    version,
    buildId,
    sourceCommit: "0a9113ebc0bd1dd284e7245acd0694027424e115",
    appPath: "tmm_v6_r0_m0.ino.bin",
    mergedPath: "tmm_v6_r0_m0.ino.merged.bin",
    appBytes: 1161808,
    appSha256: "665478b9802b191f9fae601a75e710faf9d140d0e935e4f62b7dcd9641d2f623",
    mergedBytes: 16777216,
    mergedSha256: "a6fce50d3d19c4c4d5e13472c00eb2654510f051d36afb49d5f811f497080018"
  });

  // Canonical format: TMM-<version without v>-<short commit>; version keeps its v.
  assert.equal(metadata.version, version);
  assert.equal(metadata.releaseId, `TMM-${version.slice(1)}-${buildId}`);
  assert.equal(metadata.releaseId, releaseIdFor(version, buildId));
  assert.equal(metadata.releaseId, "TMM-0.6.2-0a9113e");
  assert.doesNotMatch(metadata.releaseId, /TMM-v/);
  for (const artifact of metadata.artifacts) {
    assert.equal(artifact.releaseId, metadata.releaseId, `${artifact.fileName} shares the release binding`);
  }
});

test("every TMM build emits both artifact types as one bound release", async () => {
  const script = await readFile(scriptUrl, "utf8");
  assert.match(script, /"app"\s+0x10000\s+"lan-ota"/, "app-only BIN for LAN OTA");
  assert.match(script, /"full"\s+0\s+"usb"/, "merged BIN for USB recovery");
  assert.match(script, /release package must bind exactly two artifacts/, "binding is enforced");
  assert.match(script, /needs one 'app' and one 'full' artifact/, "both image types are mandatory");
  assert.match(script, /Could not read TMM_M0_VERSION_\* macros/, "version comes from the embedded header");
});

test("build fails when an artifact or its checksum/metadata drifts", async () => {
  const script = await readFile(scriptUrl, "utf8");
  assert.match(script, /app-only BIN not produced/);
  assert.match(script, /merged BIN not produced/);
  assert.match(script, /bound artifact missing/);
  assert.match(script, /size drift for/);
  assert.match(script, /SHA-256 drift for/);
  assert.match(script, /metadata version does not match the compiled firmware version/);
  assert.match(script, /merged BIN must cover the full 16MB flash/);
});

test("metadata records identity and supported method per BIN", async () => {
  const script = await readFile(scriptUrl, "utf8");
  for (const field of ["fileName", "sizeBytes", "sha256", "imageType", "offset", "transport", "usbRecovery", "releaseId"]) {
    assert.match(script, new RegExp(`\\b${field}\\s*=`), `artifact field ${field} is recorded`);
  }
  assert.match(script, /FlashSize=16M,FlashMode=qio/, "board-proven flash geometry (D-026)");
  assert.match(script, /partitionScheme = "tmm-ota-4mb"/);
  assert.match(script, /hardwareRevision = "TMM_V6_R0_M0"/);
});

test("metadata firmware version stays consistent with the compiled header", async () => {
  const [script, header] = await Promise.all([
    readFile(scriptUrl, "utf8"),
    readFile(versionHeaderUrl, "utf8")
  ]);
  const major = header.match(/#define\s+TMM_M0_VERSION_MAJOR\s+(\d+)/)?.[1];
  const minor = header.match(/#define\s+TMM_M0_VERSION_MINOR\s+(\d+)/)?.[1];
  const patch = header.match(/#define\s+TMM_M0_VERSION_PATCH\s+(\d+)/)?.[1];
  assert.ok(major && minor && patch, "version header defines the three macros");
  assert.ok(
    script.includes("$defines.TMM_M0_VERSION_MAJOR).$($defines.TMM_M0_VERSION_MINOR).$($defines.TMM_M0_VERSION_PATCH)"),
    "the script derives the version from the header macros"
  );
  assert.match(script, /version = \$version/, "metadata carries the compiled version");
});
