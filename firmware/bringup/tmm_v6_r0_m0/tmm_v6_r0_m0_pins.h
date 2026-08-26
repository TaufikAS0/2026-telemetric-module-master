#pragma once

// Evidence source: Modul Master.xlsx, sheet TMM_V6_R0_M0 (gid 638455439).
// This header is for non-production board bring-up only.
namespace tmm_m0 {

constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;

constexpr int SPI_MOSI = 11;
constexpr int SPI_SCK = 12;
constexpr int SPI_MISO = 13;
constexpr int SD_CS = 47;
constexpr int ETH_CS = 10;
constexpr int ETH_INT = 48;

constexpr int LORA_RX = 5;
constexpr int LORA_TX = 4;
constexpr int LORA_ACK = 1;
constexpr int LORA_AUX = 2;
constexpr int LORA_LINK = 6;
constexpr int LORA_M0 = 7;
constexpr int LORA_M1 = 14;

constexpr int RS485_RX = 17;
constexpr int RS485_TX = 18;

constexpr int SELECTOR_DMM = 40;
constexpr int SELECTOR_MMA = 41;
constexpr int SELECTOR_WEBSERVER = 39;
constexpr int CHANGE_DISPLAY = 42;
constexpr int BOOT_BUTTON = 0;

constexpr unsigned char EXPECTED_I2C_ADDRESSES[] = {
  0x20, 0x24, 0x28, 0x38, 0x57, 0x68, 0x77
};

}  // namespace tmm_m0
