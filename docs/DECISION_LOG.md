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
- Safety boundary: Do not transmit over LoRa/RS485 or write MCP1/MCP2, Ethernet, or ATtiny404 registers until their exact contracts are confirmed or a later bounded decision records a specific bring-up exception.
- Toolchain assumption: The MVP is an Arduino sketch for convenient bring-up only. The production framework remains undecided.
- Consequence: The sketch can establish device presence and wiring continuity but cannot validate every peripheral function.

## D-006 — MCP1B4 watchdog heartbeat precedes Wi-Fi

- Status: Accepted for MVP bring-up
- Hardware evidence: M0 workbook maps `DONE TPL` to MCP1B4; human operator states the board resets when it does not receive a heartbeat within five seconds.
- Component assumption: MCP1 is provisionally treated as MCP23017 at I2C address 0x20 using BANK=0 registers. Confirm from BOM/schematic before powering hardware.
- Decision: Configure MCP1B4 and issue a 100 microsecond DONE pulse every 1000 ms. Initialize it before Serial, SPI, or Wi-Fi.
- Timing evidence: TI TPL5010 specifies a minimum DONE pulse width of 100 ns. The MVP uses a 100 microsecond pulse for margin.
- Wi-Fi rule: Connection and retry logic must remain nonblocking and must never contain a wait-until-connected loop.
- Security rule: Wi-Fi credentials must not be compiled into the sketch or release artifacts.

## D-007 — Runtime Wi-Fi provisioning for bring-up

- Status: Accepted for MVP bring-up only
- Evidence: The generic compiled BIN previously contained the locally supplied SSID and password when `wifi_secrets.h` was included at compile time.
- Decision: Remove compile-time Wi-Fi credentials. Accept `wifi-set <ssid>|<password>` over the local serial console, store the values in ESP32 Preferences/NVS, and provide `wifi-clear` to erase them.
- Watchdog rule: NVS access and Wi-Fi startup remain bounded operations with heartbeat servicing immediately before and after NVS writes. Connection and retry remain nonblocking.
- Disclosure rule: Status output reports only provisioned/connected state, IP address, and RSSI; it never prints the SSID or password.
- Security limitation: Serial provisioning and the NVS partition are not assumed encrypted or production-secure. The production provisioning, device identity, flash-encryption, and credential-rotation contracts remain unknown.
- Consequence: One generic bring-up BIN can be built and distributed without embedding a site credential, while each bench device is configured after flashing.

