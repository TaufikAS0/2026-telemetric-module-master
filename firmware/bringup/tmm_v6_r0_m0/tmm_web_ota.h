#pragma once

#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>

// Reusable authenticated application-BIN uploader for ESP32 WebServer portals.
// Credential storage remains the responsibility of the product firmware.
class TmmWebOtaUploader {
 public:
  using ServiceCallback = void (*)();

  void begin(WebServer &server, const String &password, ServiceCallback serviceCallback) {
    server_ = &server;
    password_ = &password;
    serviceCallback_ = serviceCallback;
    server.on("/api/ota/upload", HTTP_POST,
      [this]() { finish(); },
      [this]() { receive(); });
  }

  void serviceRestart() {
    if (!restartAtMs_ || static_cast<int32_t>(millis() - restartAtMs_) < 0) return;
    service();
    ESP.restart();
  }

 private:
  WebServer *server_ = nullptr;
  const String *password_ = nullptr;
  ServiceCallback serviceCallback_ = nullptr;
  bool authorized_ = false;
  bool succeeded_ = false;
  uint32_t restartAtMs_ = 0;

  void service() {
    if (serviceCallback_) serviceCallback_();
  }

  void finish() {
    if (!authorized_) {
      server_->send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
    } else if (!succeeded_) {
      server_->send(422, "application/json", "{\"ok\":false,\"error\":\"update_rejected\"}");
    } else {
      server_->send(200, "application/json", "{\"ok\":true,\"state\":\"restarting\"}");
      restartAtMs_ = millis() + 1200;
    }
    authorized_ = false;
    succeeded_ = false;
  }

  void receive() {
    HTTPUpload &upload = server_->upload();
    if (upload.status == UPLOAD_FILE_START) {
      authorized_ = password_->length() >= 8 && server_->arg("password") == *password_;
      succeeded_ = false;
      if (!authorized_) return;
      service();
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) authorized_ = false;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (!authorized_) return;
      service();
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.abort();
        authorized_ = false;
      }
      service();
    } else if (upload.status == UPLOAD_FILE_END) {
      if (!authorized_) return;
      service();
      succeeded_ = Update.end(true);
      service();
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      if (Update.isRunning()) Update.abort();
      authorized_ = false;
      succeeded_ = false;
    }
  }
};
