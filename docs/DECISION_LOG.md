# Decision Log

## D-001 — Product-specific repository

- Status: Accepted
- Decision: TMM receives its own repository and independent version history.
- Reason: Product releases, issues, and hardware compatibility must remain traceable.
- Trade-off: Shared behavior must not be duplicated across product repos.

## D-002 — Shared embedded core later

- Status: Accepted in principle
- Decision: Common identity, protocol, logging, diagnostics, and update behavior will live in a separately versioned library.
- Reason: Copying common code into each product creates incompatible forks.
- Unknown: Package mechanism will be selected after the embedded toolchain is confirmed.

## D-003 — Desktop simulation before production firmware

- Status: Accepted
- Decision: Validate state models, APIs, operator workflow, and tests on Windows first.
- Reason: Hardware facts are currently missing.
- Limitation: This does not validate electrical behavior or real-time constraints.

## D-004 — Portal controls approval

- Status: Accepted
- Decision: GitHub stores source and release artifacts; the local hardware portal controls approved/recommended status and QC history.
- Reason: Code publication and hardware release approval are different responsibilities.

## D-005 — M0 passive bring-up before active drivers

- Status: Accepted for MVP bring-up
- Evidence: `Modul Master.xlsx`, tab `TMM_V6_R0_M0`, received 2026-08-26.
- Decision: Capture the verified M0 pin map in a machine-readable profile and begin with passive GPIO reads, I2C discovery, and an operator-triggered SD probe.
- Reason: The workbook confirms bus pins but does not identify several device types or communication contracts.
- Safety boundary: Do not transmit over LoRa/RS485 or write MCP1/MCP2, Ethernet, or ATtiny404 registers until their exact contracts are confirmed.
- Toolchain assumption: The MVP is an Arduino sketch for convenient bring-up only. The production framework remains undecided.
- Consequence: The sketch can establish device presence and wiring continuity but cannot validate every peripheral function.

