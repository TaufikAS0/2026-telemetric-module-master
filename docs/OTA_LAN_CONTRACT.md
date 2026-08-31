# TMM LAN OTA Contract (draft, bring-up only)

Status: draft, no hardware evidence. This document defines the simple contract
that lets the Telemetric Hardware Portal backend find and update a TMM device
over the LAN. Decisions: D-023, D-024, D-025. Compile and simulator evidence
only — a physical board has not been updated through this path yet.

## 1. Device identity

`GET /api/device-info` (HTTP port 80, read-only, no authentication) returns:

```json
{
  "productCode": "TMM",
  "deviceId": "<12 hex from efuse MAC>",
  "hardwareRevision": "TMM_V6_R0_M0",
  "firmwareVersion": "v0.6.0",
  "chipFamily": "ESP32-S3",
  "flashSize": "16MB",
  "flashMode": "qio",
  "partitionScheme": "tmm-ota-4mb",
  "ip": "192.168.1.50",
  "otaSupported": true,
  "otaPort": 80,
  "otaPath": "/api/ota/image",
  "mdns": { "service": "_telemetric-ota._tcp", "hostname": "tmm-v6-r0-m0" }
}
```

`deviceId` is the ESP32 eFuse MAC (12 lowercase hex characters). It is bench
identity evidence, not a provisioned cryptographic identity (D-025).

`otaSupported` is `true` only when the device is advertised over mDNS on the
station network AND an OTA token of at least 8 characters has been provisioned.

## 2. Discovery

After the station Wi-Fi connects, the device advertises mDNS service
`_telemetric-ota._tcp` on port 80 with hostname `tmm-v6-r0-m0` and TXT records:
`productCode`, `deviceId`, `hwRev`, `fwVer`, `chipFamily`, `flashSize`, `path`
(`/api/device-info`), `otaPath` (`/api/ota/image`), `otaPort` (`80`).

The portal backend can also skip mDNS and query `http://<ip>/api/device-info`
directly; the JSON is authoritative.

## 3. Update endpoint

`POST /api/ota/image` (HTTP port 80, `multipart/form-data` with one file
field) writes the uploaded application image to the inactive OTA slot with the
ESP32 `Update` API and reboots into it on success.

- Authentication: `Authorization: Bearer <token>` (or `X-OTA-Token: <token>`),
  compared constant-time against the NVS-provisioned OTA token. Missing or
  wrong token → `401 {"ok":false,"error":"unauthorized"}`.
- The token is never hard-coded. It is provisioned on the bench through the
  115200-baud serial console (`ota-set <password>`, stored in the `tmm-ota`
  NVS namespace) and shared with the portal out of band. `ota-clear` revokes it.
- Image size gate: an image larger than the free OTA slot (a 4 MB merged
  image) is rejected before writing — LAN OTA accepts the app-only BIN only.
- Response codes: `200 {"ok":true,"state":"restarting"}` on success; `401`
  unauthorized; `422` update rejected/invalid image; `413` image larger than
  the OTA slot.

## 4. Partitions and rollback

The build uses `partitions.csv` in the sketch folder with the safe OTA layout
(decision D-024): `nvs`, `otadata`, `ota_0` (1984K), `ota_1` (1984K),
`coredump`. Both app slots are interchangeable; `otadata` selects the slot.

Board-proven module geometry (decision D-026): the ESP32-S3 module on the
bench device carries 16 MB flash in QIO mode, confirmed live through
`GET /api/device-info` after a real LAN OTA. The partition table itself is
**not changed by an app-only OTA** — `Update` with `U_FLASH` writes only the
inactive app slot; the table at `0x8000` and `otadata` stay untouched. The
table above is byte-identical to the one already running on the device (its
slots live inside the first 4 MB, which is valid on the 16 MB module), so
adopting the 16MB/QIO profile does **not** require another USB merged flash;
subsequent updates can continue over LAN. A USB merged flash is only needed
if the partition layout itself ever changes.

Rollback: when the bootloader is built with app-rollback support, the newly
written slot boots in the pending-verify state and is only confirmed after
`TmmLanOta::STABILITY_CONFIRM_MS` (15 s) of continuously healthy loop
iterations (watchdog heartbeat alive). A device that crashes or resets before
confirmation boots back into the previous slot. With the stock Arduino
bootloader (rollback disabled) the flag is compiled out; `Update.end(true)`
still validates the image before switching.

## 5. Artifacts

Every build produces two artifact types with distinguishing metadata
(`artifacts.json`, decision D-024). The desktop contract simulator mirrors
this shape (`simulator/ota-lan.mjs`, `buildArtifactMetadata`):

| imageType | File | Offset | Transport | Use |
|---|---|---|---|---|
| `app` | `tmm_v6_r0_m0.ino.bin` | `0x10000` | LAN OTA (`/api/ota/image`) | In-field update |
| `full` | `tmm_v6_r0_m0.ino.merged.bin` | `0` | USB flashing | First flash and USB recovery (16 MB image on the proven module) |

USB recovery is preserved: the merged BIN always exists and must never be
sent to the OTA endpoint.

## 6. Evidence limits

- Compile + partition-table inspection and the simulator tests prove the
  contract shapes only.
- Proven on hardware (2026-08-31, bench device `192.168.1.53`,
  deviceId `9830bcad4f7c`): real app-only OTA over LAN succeeded and
  `GET /api/device-info` reports the identity document including
  16 MB/QIO flash geometry.
- Still not proven: mDNS browsing from a real backend host, rollback
  behavior under a real crash, and PSRAM/RAM geometry (see
  `docs/HARDWARE_DISCOVERY.md`).
