# AI Handoff — Telemetric Module Master

## Mission

Develop the Telemetric Module Master through evidence-based phases. The first
deliverable is a testable desktop model. Production embedded firmware starts
only after the hardware and communication contracts are confirmed.

## Current state

- Product code: `TMM`
- Product name: `Telemetric Module Master`
- Category: `Main Products / Monitoring & Control`
- Current maturity: Phase 0, desktop simulator
- Production MCU: partial evidence for ESP32-S3 + ATtiny404; exact ESP32-S3 part/module unknown
- PCB revision: TMM V6 R0 mode M0 identified by workbook; schematic revision unknown
- Physical interfaces: unknown
- Production firmware status: not started
- Bring-up firmware status: passive M0 diagnostic MVP added; not production firmware

## Decisions already made

- TMM has its own product repository and release history.
- Reusable firmware capabilities must eventually live in a versioned shared library, not copied into every product.
- Release artifacts will include a merged BIN, manifest, SHA-256 file, and release notes.
- The hardware portal remains the approval and QC authority. A GitHub Release is not automatically approved firmware.
- Desktop simulation and real hardware verification are separate evidence levels.

## Current simulator contract

The simulator represents a master controller with virtual subordinate modules.
It exposes health, module inventory, event history, polling, and online/offline
state transitions through local HTTP APIs.

This contract may guide firmware architecture, but it must not be treated as a
confirmed physical communication protocol.

## Required reading order for another AI

1. `AGENTS.md`
2. This document
3. `docs/PRD.md`
4. `docs/HARDWARE_DISCOVERY.md`
5. `docs/DECISION_LOG.md`
6. `docs/ACCEPTANCE_TESTS.md`
7. Existing code and tests

## Handoff completion format

Every AI work session must finish with:

- Summary of changes
- Tests executed and exact results
- Files changed
- Facts established
- Assumptions introduced
- Unknowns remaining
- Recommended next task
- Git branch and commit hash, if committed

