#pragma once
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Measurement.h"
#include "DataLogger.h"
class MqttClientMgr;

class WebServerMgr {
public:
  explicit WebServerMgr(uint16_t port = 80) : _server(port) {}

  void begin(const Measurement* latest, DataLogger* logger, MqttClientMgr* mqtt);
  void loop();

private:
  ESP8266WebServer _server;
  const Measurement* _latest = nullptr;
  DataLogger* _logger = nullptr;
  MqttClientMgr* _mqtt = nullptr;

  void handleHealth();
  void handleLatest();
  void handleLogsList();
  void handleLogsDownload();
  void handleLogsDownloadAll();
  void handleLogsRange();
  void serveStaticFiles();
  void handleLogsClear();
  void handleMqttGet();
  void handleMqttSave();
  void handleDeviceInfo();
  void handleMqttStatus();
};
