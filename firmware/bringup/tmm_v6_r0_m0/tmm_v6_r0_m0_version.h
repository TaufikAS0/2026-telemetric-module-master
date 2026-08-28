#pragma once

// Dedicated embedded source of truth for the TMM V6 R0 M0 bring-up firmware
// semantic version (decision D-014).
//
// - v0.2.0 carries the v0.1.0 bench-session corrections: explicit active-low
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
#define TMM_M0_VERSION_MINOR 2
#define TMM_M0_VERSION_PATCH 0

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
