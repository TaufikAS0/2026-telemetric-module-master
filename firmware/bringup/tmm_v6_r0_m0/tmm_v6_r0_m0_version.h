#pragma once

// Dedicated embedded source of truth for the TMM V6 R0 M0 bring-up firmware
// semantic version (decision D-014).
//
// - v0.6.2 is the Hardware QC release: it keeps every proven QC capability of
//   v0.6.0/v0.6.1 (six mandatory items per D-012/D-015/D-016/D-017/D-020,
//   portal + LAN OTA per D-023/D-024/D-025) and adds a machine-readable
//   `qcSummary` roll-up to /api/status (D-027) that distinguishes untested /
//   testing / pass / fail / manual per item. PASS still comes only from the
//   operator decision recorded in the portal — the summary never converts
//   compile success or live evidence into an automatic hardware PASS.
// - v0.6.1 corrects the build compatibility profile to the board-proven
//   module flash geometry, 16 MB / QIO (D-026), and rebuilds both artifacts
//   with identical OTA partition offsets. It is the artifact-bearing
//   successor to v0.6.0 so the two BIN families can never be confused.
// - v0.6.0 carries the RS485 Modbus RTU QC card (D-020) and the interactive
//   clickable operator tutorial (D-021): a bench-proven Modbus RTU master
//   ported from the Longhi hwtest onto the workbook RS485 pins (RX GPIO17 /
//   TX GPIO18, 9600 8N1, auto+manual poll, 3-valid streak PASS), plus
//   tutorial steps and progress tiles that reveal their QC card, live
//   per-step state badges, and a running-test indicator. Per D-022 it is the
//   feature-bearing successor to v0.5.0, the latest approved release in the
//   TMM product registry; the earlier v0.3.1 identity collided with
//   historical registry releases and was corrected, not a feature removal.
// - v0.2.0 carried the v0.1.0 bench-session corrections: explicit active-low
//   LED drive with a manual per-LED mode (D-015), datasheet-ordered AHT10
//   init plus stage diagnostics (D-016), SD card-presence probe with
//   granular errors (D-017), and the two-column QC portal layout (D-018).
//   Still explicitly non-production: it carries no completed hardware QC.
// - v0.1.0 was the initial feature-bearing bring-up version.
// - The desktop simulator identity lives in /package.json and is deliberately
//   independent; firmware must never read the simulator version and the
//   simulator must never define an embedded version.
// - Every runtime surface (serial profile JSON, /api/status, QC portal page)
//   consumes only FIRMWARE_VERSION from this header.

// The components must be preprocessor macros, not constexpr variables: the
// stringify helpers below expand the *token* they receive, so stringifying a
// constexpr identifier would bake the literal macro name into FIRMWARE_VERSION.
#define TMM_M0_VERSION_MAJOR 0
#define TMM_M0_VERSION_MINOR 6
#define TMM_M0_VERSION_PATCH 2

#define TMM_M0_VERSION_STRINGIFY_(value) #value
#define TMM_M0_VERSION_STRINGIFY(value) TMM_M0_VERSION_STRINGIFY_(value)

namespace tmm_m0 {

constexpr unsigned int FIRMWARE_VERSION_MAJOR = TMM_M0_VERSION_MAJOR;
constexpr unsigned int FIRMWARE_VERSION_MINOR = TMM_M0_VERSION_MINOR;
constexpr unsigned int FIRMWARE_VERSION_PATCH = TMM_M0_VERSION_PATCH;

inline constexpr const char FIRMWARE_VERSION[] =
    "v" TMM_M0_VERSION_STRINGIFY(TMM_M0_VERSION_MAJOR) "." TMM_M0_VERSION_STRINGIFY(
        TMM_M0_VERSION_MINOR) "." TMM_M0_VERSION_STRINGIFY(TMM_M0_VERSION_PATCH);

}  // namespace tmm_m0

#undef TMM_M0_VERSION_STRINGIFY_
#undef TMM_M0_VERSION_STRINGIFY
