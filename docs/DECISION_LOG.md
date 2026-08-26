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

