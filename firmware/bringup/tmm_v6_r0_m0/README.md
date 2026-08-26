# TMM V6 R0 M0 bring-up MVP

This Arduino sketch is a deliberately conservative diagnostic for the
`TMM_V6_R0_M0` PCB. It is not production firmware and does not prove electrical
compatibility.

## Before flashing

Confirm the exact ESP32-S3 module, flash/PSRAM settings, USB/serial upload path,
power limits, and recovery procedure. Select those settings in Arduino IDE from
the physical module or schematic; this repository intentionally does not guess
a board profile.

## Safe default behavior

At boot, the sketch:

- starts I2C immediately and configures MCP1B4 as the watchdog heartbeat;
- pulses MCP1B4 every second, including during I2C scans and Wi-Fi retries;
- starts the USB/serial console at 115200 baud;
- configures evidence-backed status GPIOs as inputs;
- starts I2C on GPIO 8/9 and SPI on GPIO 11/12/13;
- holds the SD and Ethernet chip-select pins high;
- connects Wi-Fi without a blocking wait loop;
- does not transmit over LoRa or RS485;
- does not drive MCP pins other than MCP1B4, or any ATtiny404 pin.

Before compiling, copy `wifi_secrets.example.h` to `wifi_secrets.h` in the same
Arduino sketch folder and fill in the local SSID and password. The real file is
ignored by Git and must not be pushed.

Serial commands:

- `profile` — show the board/evidence level;
- `wifi` — show connection state, local IP, and RSSI without revealing credentials;
- `heartbeat` — show MCP1B4 heartbeat readiness and interval;
- `scan-i2c` — list every responding I2C address;
- `check-i2c` — check the seven addresses stated in the workbook;
- `gpio` — read the direct input pins without changing them;
- `blocked` — list drivers waiting for hardware evidence;
- `help` — list commands.

## Why some hardware is scan-only

The workbook does not identify the exact MCP1/MCP2 or Ethernet controller, and
does not provide LoRa/RS485 baud or protocol contracts. The ATtiny404 section
also contains a 3.3 V versus 1.8 V conflict. Adding active drivers before those
facts are resolved could write the wrong registers or drive unsafe levels.

The heartbeat implementation provisionally treats MCP1 as an MCP23017 at
address `0x20`, using BANK=0 register addresses. This is limited to MCP1B4 and
must be checked against the BOM or schematic before the first powered test.
