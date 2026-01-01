#pragma once

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Measurement.h"

class MqttClientMgr {
public:
  MqttClientMgr();

  void begin(const Measurement* latest);
  void loop();
  const String& lastLog() const;

private:
  void loadConfigIfNeeded();
  bool connectIfNeeded();
  void publishDiscovery();
  void publishState();
  void configureClient();
  String chipIdHex() const;
  void logLine(const String& line);

  WiFiClient _wifi;
  PubSubClient _client;
  const Measurement* _latest = nullptr;

  String _server;
  uint16_t _port = 0;
  String _user;
  String _pass;
  bool _configured = false;

  unsigned long _lastConfigCheck = 0;
  unsigned long _lastReconnectAttempt = 0;
  unsigned long _lastPublish = 0;
  bool _discoveryPublished = false;
  String _lastLog;
  uint8_t _failCount = 0;
  unsigned long _nextRetryAt = 0;

  String _clientId;
  String _baseTopic;
  String _availabilityTopic;
};
