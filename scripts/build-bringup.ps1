# Builds the TMM V6 R0 M0 bring-up firmware with the safe OTA partition table
# (decision D-024) and emits both artifact types with distinguishing metadata:
# an app-only BIN for LAN OTA and a merged BIN for USB flashing/recovery.
# BIN files stay in the (git-ignored) build output directory; never commit them.
#
# Compile-only evidence: this proves toolchain consistency, not board
# compatibility. The exact ESP32-S3 module/flash settings still need physical
# confirmation before any real flash session.
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

$compileArgs = @(
  "compile",
  "--fqbn", "esp32:esp32:esp32s3:FlashSize=4M,FlashMode=dio,PartitionScheme=custom",
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
  }
}

$metadata = [ordered]@{
  schemaVersion = 1
  productCode = "TMM"
  buildId = "$buildId-lan-ota"
  sourceCommit = $sourceCommit
  hardwareRevision = "TMM_V6_R0_M0"
  chipFamily = "ESP32-S3"
  flashSize = "4MB"
  flashMode = "dio"
  partitionScheme = "tmm-ota-4mb"
  fqbn = "esp32:esp32:esp32s3:FlashSize=4M,FlashMode=dio,PartitionScheme=custom"
  artifacts = @(
    Get-ArtifactInfo $appBin "app" 0x10000 "lan-ota" $false
    Get-ArtifactInfo $mergedBin "full" 0 "usb" $true
  )
}

$metadataPath = Join-Path $outPath "artifacts.json"
$metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8
Write-Host "Artifacts metadata: $metadataPath"
Get-Content -LiteralPath $metadataPath
