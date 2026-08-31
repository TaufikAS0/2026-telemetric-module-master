#pragma once

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_ota_ops.h>

// LAN OTA contract for the Telemetric Hardware Portal backend (decision D-023).
// The device advertises itself over mDNS as _telemetric-ota._tcp, exposes a
// read-only identity document at GET /api/device-info, and accepts an
// application image at POST /api/ota/image guarded by a bearer token that is
// provisioned in NVS through the serial console (ota-set); no credential is
// hard-coded here. The full contract is documented in
// docs/OTA_LAN_CONTRACT.md.

struct TmmOtaLanIdentity {
  const char *productCode;
  const char *deviceId;            // 12 lowercase hex characters from efuse MAC
  const char *hardwareRevision;
  const char *firmwareVersion;
  const char *chipFamily;
  const char *partitionScheme;
  const char *hostname;
};

class TmmLanOta {
 public:
  using PasswordProvider = const String & (*)();

  static constexpr const char *kDeviceInfoPath = "/api/device-info";
  static constexpr const char *kOtaImagePath = "/api/ota/image";
  static constexpr const char *kServiceType = "_telemetric-ota";
  static constexpr const char *kServiceProto = "_tcp";

  void begin(WebServer &server, const TmmOtaLanIdentity &identity, PasswordProvider passwordProvider) {
    server_ = &server;
    identity_ = identity;
    passwordProvider_ = passwordProvider;
    // Collect the token headers before webServer.begin(); WebServer ignores
    // headers it was not told to collect.
    const char *headerKeys[] = {"Authorization", "X-OTA-Token"};
    server_->collectHeaders(headerKeys, 2);
    server_->on(kDeviceInfoPath, HTTP_GET, [this]() { sendDeviceInfo(); });
    server_->on(kOtaImagePath, HTTP_POST,
      [this]() { finishUpload(); },
      [this]() { receiveUpload(); });
  }

  // Advertise over mDNS on the station network. Returns false when the
  // station interface is not ready or mDNS could not start.
  bool advertise() {
    if (advertised_) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!MDNS.begin(identity_.hostname)) {
      Serial.println(F("{\"lanOta\":{\"event\":\"mdns-failed\"}}"));
      return false;
    }
    char flashSize[12];
    const uint32_t flashBytes = ESP.getFlashChipSize();
    snprintf(flashSize, sizeof(flashSize), "%uMB", static_cast<unsigned>(flashBytes / (1024U * 1024U)));
    MDNS.addService(kServiceType + 1, kServiceProto + 1, 80);
    addTxt("productCode", identity_.productCode);
    addTxt("deviceId", identity_.deviceId);
    addTxt("hwRev", identity_.hardwareRevision);
    addTxt("fwVer", identity_.firmwareVersion);
    addTxt("chipFamily", identity_.chipFamily);
    addTxt("flashSize", flashSize);
    addTxt("path", kDeviceInfoPath);
    addTxt("otaPath", kOtaImagePath);
    addTxt("otaPort", "80");
    advertised_ = true;
    Serial.printf(
      "{\"lanOta\":{\"ready\":true,\"service\":\"%s.%s\",\"hostname\":\"%s\",\"deviceId\":\"%s\",\"ip\":\"%s\"}}\n",
      kServiceType, kServiceProto, identity_.hostname, identity_.deviceId,
      WiFi.localIP().toString().c_str());
    return true;
  }

  void end() {
    if (!advertised_) return;
    MDNS.end();
    advertised_ = false;
  }

  bool advertised() const { return advertised_; }

  // Rollback confirmation (decision D-024): when the bootloader was built
  // with app-rollback support, the new OTA slot starts in the pending-verify
  // state and is only confirmed after STABILITY_CONFIRM_MS of continuously
  // healthy loop iterations. A device that crashes or watchdog-resets before
  // confirmation boots back into the previous OTA slot.
  static constexpr uint32_t STABILITY_CONFIRM_MS = 15000;

  void serviceStabilityConfirm(bool loopHealthy) {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE == 1
    if (stabilityConfirmed_ || !loopHealthy) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - stabilityStartMs_) < 0) stabilityStartMs_ = now;
    if (static_cast<int32_t>(now - stabilityStartMs_) < static_cast<int32_t>(STABILITY_CONFIRM_MS)) return;
    esp_ota_mark_app_valid_cancel_rollback();
    stabilityConfirmed_ = true;
    Serial.println(F("{\"otaRollback\":{\"state\":\"confirmed\"}}"));
#else
    (void)loopHealthy;
#endif
  }

  bool rollbackConfirmed() const {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE == 1
    return stabilityConfirmed_;
#else
    return false;
#endif
  }

  // Builds the identity document. Kept public so the serial console can print
  // the exact document that the HTTP endpoint serves.
  void buildDeviceInfoJson(char *buffer, const size_t length) const {
    char flashSize[12];
    const uint32_t flashBytes = ESP.getFlashChipSize();
    snprintf(flashSize, sizeof(flashSize), "%uMB", static_cast<unsigned>(flashBytes / (1024U * 1024U)));
    const bool tokenReady = passwordProvider_ && passwordProvider_().length() >= kPasswordMinLength;
    const IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
    const char *flashMode;
    switch (ESP.getFlashChipMode()) {
      case FM_QIO: flashMode = "qio"; break;
      case FM_QOUT: flashMode = "qout"; break;
      case FM_DIO: flashMode = "dio"; break;
      case FM_DOUT: flashMode = "dout"; break;
      case FM_FAST_READ: flashMode = "fast-read"; break;
      case FM_SLOW_READ: flashMode = "slow-read"; break;
      default: flashMode = "unknown"; break;
    }
    snprintf(buffer, length,
      "{\"productCode\":\"%s\",\"deviceId\":\"%s\",\"hardwareRevision\":\"%s\","
      "\"firmwareVersion\":\"%s\",\"chipFamily\":\"%s\",\"flashSize\":\"%s\",\"flashMode\":\"%s\","
      "\"partitionScheme\":\"%s\",\"ip\":\"%s\",\"otaSupported\":%s,\"otaPort\":80,"
      "\"otaPath\":\"%s\",\"mdns\":{\"service\":\"%s.%s\",\"hostname\":\"%s\"}}",
      identity_.productCode, identity_.deviceId, identity_.hardwareRevision,
      identity_.firmwareVersion, identity_.chipFamily, flashSize, flashMode,
      identity_.partitionScheme, ip.toString().c_str(),
      advertised_ && tokenReady ? "true" : "false", kOtaImagePath,
      kServiceType, kServiceProto, identity_.hostname);
  }

 private:
  static constexpr size_t kPasswordMinLength = 8;

  WebServer *server_ = nullptr;
  TmmOtaLanIdentity identity_{};
  PasswordProvider passwordProvider_ = nullptr;
  bool advertised_ = false;
  bool authorized_ = false;
  bool succeeded_ = false;
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE == 1
  bool stabilityConfirmed_ = false;
  uint32_t stabilityStartMs_ = 0;
#endif

  void addTxt(const char *key, const char *value) {
    MDNS.addServiceTxt(kServiceType + 1, kServiceProto + 1, key, value);
  }

  bool tokenMatches(const String &expected, const String &provided) const {
    if (expected.length() < kPasswordMinLength || expected.length() != provided.length()) return false;
    // Constant-time comparison; bench-grade but avoids a trivial timing oracle.
    volatile uint8_t difference = 0;
    for (size_t index = 0; index < expected.length(); ++index) {
      difference |= static_cast<uint8_t>(expected[index]) ^ static_cast<uint8_t>(provided[index]);
    }
    return difference == 0;
  }

  bool requestAuthorized() {
    if (!passwordProvider_) return false;
    const String &expected = passwordProvider_();
    String provided = server_->header("Authorization");
    if (provided.startsWith("Bearer ")) provided = provided.substring(7);
    else {
      provided = server_->header("X-OTA-Token");
      if (!provided.length()) return false;
    }
    provided.trim();
    return tokenMatches(expected, provided);
  }

  void receiveUpload() {
    HTTPUpload &upload = server_->upload();
    if (upload.status == UPLOAD_FILE_START) {
      authorized_ = requestAuthorized();
      succeeded_ = false;
      if (!authorized_) return;
      const esp_partition_t *running = esp_ota_get_running_partition();
      const uint32_t freeBytes = running ? running->size : 0;
      if (freeBytes && upload.totalSize > freeBytes) {
        // A full merged image (bootloader + partition table + app) does not
        // fit into one OTA slot; it must be flashed over USB instead.
        authorized_ = false;
        Serial.printf(
          "{\"lanOta\":{\"event\":\"rejected\",\"reason\":\"image_larger_than_ota_slot\",\"imageBytes\":%u,\"slotBytes\":%u}}\n",
          static_cast<unsigned>(upload.totalSize), static_cast<unsigned>(freeBytes));
        return;
      }
      Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
      authorized_ = Update.isRunning();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (!authorized_) return;
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.abort();
        authorized_ = false;
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!authorized_) return;
      succeeded_ = Update.end(true);
      Serial.printf(
        "{\"lanOta\":{\"event\":\"%s\",\"bytes\":%u}}\n",
        succeeded_ ? "complete" : "failed", static_cast<unsigned>(upload.totalSize));
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (Update.isRunning()) Update.abort();
      authorized_ = false;
      succeeded_ = false;
    }
  }

  void finishUpload() {
    if (!authorized_) {
      server_->send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    } else if (!succeeded_) {
      server_->send(422, "application/json", "{\"ok\":false,\"error\":\"update_rejected\"}");
    } else {
      server_->send(200, "application/json", "{\"ok\":true,\"state\":\"restarting\"}");
      delay(50);
      ESP.restart();
    }
    authorized_ = false;
    succeeded_ = false;
  }

  void sendDeviceInfo() {
    char document[512];
    buildDeviceInfoJson(document, sizeof(document));
    server_->sendHeader(F("Cache-Control"), F("no-store"));
    server_->send(200, "application/json", document);
  }
};
