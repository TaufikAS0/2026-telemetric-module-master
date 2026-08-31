# Builds the TMM V6 R0 M0 bring-up firmware with the safe OTA partition table
# (decision D-024) and emits both artifact types as ONE bound release:
# an app-only BIN for LAN OTA and a merged BIN for USB flashing/recovery.
# A build FAILS unless both BIN files exist and every recorded size and
# SHA-256 re-verifies against the physical files.
# BIN files stay in the (git-ignored) build output directory; never commit them.
#
# Compile-only evidence: this proves toolchain consistency, not board
# compatibility. Flash geometry 16 MB / QIO is board-proven (decision D-026).
param(
  [string]$OutputDir = "firmware/bringup/build/tmm_v6_r0_m0"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$outPath = Join-Path $repoRoot $OutputDir
New-Item -ItemType Directory -Force -Path $outPath | Out-Null

# The esp32 core's prebuild hook copies the sketch-folder partitions.csv when
# PartitionScheme=custom, so the safe OTA table (decision D-024) travels with
# the sketch and no build-property override is needed.
$buildId = git -C $repoRoot rev-parse --short=7 HEAD
$sourceCommit = git -C $repoRoot rev-parse HEAD

# The firmware semantic version is read from its embedded source of truth so
# the metadata can never drift from the compiled image.
$versionHeader = Join-Path $repoRoot "firmware\bringup\tmm_v6_r0_m0\tmm_v6_r0_m0_version.h"
$defines = @{}
Get-Content -LiteralPath $versionHeader | ForEach-Object {
  if ($_ -match '^\s*#define\s+(TMM_M0_VERSION_(?:MAJOR|MINOR|PATCH))\s+(\d+)\s*$') {
    $defines[$matches[1]] = [int]$matches[2]
  }
}
if ($defines.Count -ne 3) { throw "Could not read TMM_M0_VERSION_* macros from $versionHeader" }
$version = "v$($defines.TMM_M0_VERSION_MAJOR).$($defines.TMM_M0_VERSION_MINOR).$($defines.TMM_M0_VERSION_PATCH)"
$releaseId = "TMM-$($version.TrimStart('v'))-$buildId"

$compileArgs = @(
  "compile",
  # 16 MB / QIO is board-proven (decision D-026); the OTA partition layout
  # itself is byte-identical to the table already flashed on the device.
  "--fqbn", "esp32:esp32:esp32s3:FlashSize=16M,FlashMode=qio,PartitionScheme=custom",
  "--output-dir", $outPath,
  (Join-Path $repoRoot "firmware\bringup\tmm_v6_r0_m0")
)
& arduino-cli @compileArgs
if ($LASTEXITCODE -ne 0) { throw "arduino-cli compile failed with exit code $LASTEXITCODE" }

$appBin = Join-Path $outPath "tmm_v6_r0_m0.ino.bin"
$mergedBin = Join-Path $outPath "tmm_v6_r0_m0.ino.merged.bin"
if (!(Test-Path $appBin)) { throw "app-only BIN not produced: $appBin" }
if (!(Test-Path $mergedBin)) { throw "merged BIN not produced: $mergedBin" }

function Get-ArtifactInfo([string]$Path, [string]$ImageType, [long]$Offset, [string]$Transport, [bool]$UsbRecovery) {
  $item = Get-Item -LiteralPath $Path
  [ordered]@{
    imageType = $ImageType
    offset = $Offset
    fileName = $item.Name
    sizeBytes = $item.Length
    sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()
    transport = $Transport
    usbRecovery = $UsbRecovery
    releaseId = $releaseId
  }
}

$metadata = [ordered]@{
  schemaVersion = 2
  productCode = "TMM"
  version = $version
  releaseId = $releaseId
  buildId = "$buildId-lan-ota"
  sourceCommit = $sourceCommit
  hardwareRevision = "TMM_V6_R0_M0"
  chipFamily = "ESP32-S3"
  flashSize = "16MB"
  flashMode = "qio"
  partitionScheme = "tmm-ota-4mb"
  fqbn = "esp32:esp32:esp32s3:FlashSize=16M,FlashMode=qio,PartitionScheme=custom"
  artifacts = @(
    Get-ArtifactInfo $appBin "app" 0x10000 "lan-ota" $false
    Get-ArtifactInfo $mergedBin "full" 0 "usb" $true
  )
}

# One release package must carry both BINs; fail the build if either is missing.
$metadataPath = Join-Path $outPath "artifacts.json"
$metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8

# Re-verify the written metadata against the physical files; any drift fails
# the build instead of publishing a mismatched artifact pair.
$check = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
if ($check.artifacts.Count -ne 2) { throw "release package must bind exactly two artifacts" }
foreach ($artifact in $check.artifacts) {
  $file = Join-Path $outPath $artifact.fileName
  if (!(Test-Path -LiteralPath $file)) { throw "bound artifact missing: $file" }
  if ((Get-Item -LiteralPath $file).Length -ne $artifact.sizeBytes) { throw "size drift for $($artifact.fileName)" }
  if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLower() -ne $artifact.sha256) { throw "SHA-256 drift for $($artifact.fileName)" }
  if ($artifact.releaseId -ne $check.releaseId) { throw "artifact $($artifact.fileName) is not bound to release $($check.releaseId)" }
}
$app = $check.artifacts | Where-Object imageType -eq "app"
$full = $check.artifacts | Where-Object imageType -eq "full"
if (!$app -or !$full) { throw "release package needs one 'app' and one 'full' artifact" }
if ($app.offset -ne 0x10000) { throw "app BIN offset must be 0x10000" }
if ($full.offset -ne 0) { throw "merged BIN offset must be 0" }
if ($full.sizeBytes -ne 16MB) { throw "merged BIN must cover the full 16MB flash" }
if ($check.version -ne $version) { throw "metadata version does not match the compiled firmware version" }

Write-Host "Release package verified: $($check.releaseId) (firmware $version, source $buildId)"
Write-Host "Artifacts metadata: $metadataPath"
Get-Content -LiteralPath $metadataPath
