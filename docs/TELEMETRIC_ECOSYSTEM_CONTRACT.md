# Telemetric Ecosystem Contract for TMM

## This repository owns

- TMM source code and source history;
- reproducible TMM build configuration;
- TMM-specific tests, hardware contract, and release notes;
- the source commit from which a candidate BIN is produced.

## This repository does not own

- the central historical BIN archive;
- portal approval or recommended status;
- production flash sessions or QC results;
- source code for unrelated Telemetric products.

## Required handoff

When TMM has a real hardware-backed build:

1. build and test from a clean, identified TMM commit;
2. generate the correct image type;
3. compute exact byte size and SHA-256;
4. prepare an evidence-backed manifest;
5. use `TaufikAS0/telemetric-firmware-library` for the immutable Release asset;
6. download the uploaded asset and verify SHA-256 again;
7. synchronize it to the local Hardware Portal;
8. allow the portal to import it as draft and control all operational approval/QC.

Do not commit BIN files here. Do not claim production readiness from simulator,
compile, upload, or flashing evidence alone.

