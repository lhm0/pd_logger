#include "MqttClientMgr.h"
#include <math.h>

static const char* kMqttConfigPath = "/mqtt.json";
static const unsigned long kConfigCheckMs = 5000;

MqttClientMgr::MqttClientMgr() : _client(_wifi) {}

void MqttClientMgr::begin(const Measurement* latest) {
  _latest = latest;
  _client.setBufferSize(512);
  _client.setSocketTimeout(2);
  _wifi.setTimeout(2000);
  _lastConfigCheck = 0;
  _lastReconnectAttempt = 0;
  _lastPublish = 0;
  _failCount = 0;
  _nextRetryAt = 0;
  logLine(String(F("[MQTT] init")));
}

void MqttClientMgr::loop() {
  _client.loop();

  loadConfigIfNeeded();
  if (!_configured) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!connectIfNeeded()) return;

  if (!_discoveryPublished) {
    publishDiscovery();
    _discoveryPublished = true;
  }

  if (millis() - _lastPublish >= SAMPLE_INTERVAL_MS) {
    _lastPublish = millis();
    publishState();
  }
}

const String& MqttClientMgr::lastLog() const {
  return _lastLog;
}

void MqttClientMgr::logLine(const String& line) {
  _lastLog = line;
  Serial.println(line);
}

void MqttClientMgr::loadConfigIfNeeded() {
  if (millis() - _lastConfigCheck < kConfigCheckMs) return;
  _lastConfigCheck = millis();

  if (!LittleFS.exists(kMqttConfigPath)) {
    if (_configured) logLine(String(F("[MQTT] config missing, disabling")));
    _configured = false;
    return;
  }

  File f = LittleFS.open(kMqttConfigPath, "r");
  if (!f) {
    logLine(String(F("[MQTT] config open failed")));
    _configured = false;
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    logLine(String(F("[MQTT] config parse failed")));
    _configured = false;
    return;
  }

  String server = doc["server"] | "";
  int port = doc["port"] | 0;
  String user = doc["user"] | "";
  String pass = doc["pass"] | "";
  if (server.length() == 0 || port <= 0 || port > 65535) {
    logLine(String(F("[MQTT] config invalid (server/port)")));
    _configured = false;
    return;
  }

  if (!_configured || server != _server || (uint16_t)port != _port || user != _user || pass != _pass) {
    _server = server;
    _port = (uint16_t)port;
    _user = user;
    _pass = pass;
    configureClient();
    _configured = true;
    logLine(String("[MQTT] config loaded: ") + _server + ":" + String(_port) +
            " user=" + (_user.length() ? _user : "(none)"));
  }
}

void MqttClientMgr::configureClient() {
  _client.setServer(_server.c_str(), _port);
  _clientId = String("pd-logger-") + chipIdHex();
  _baseTopic = String("pd_logger/") + chipIdHex();
  _availabilityTopic = _baseTopic + "/availability";
  _discoveryPublished = false;
  _failCount = 0;
  _nextRetryAt = 0;
  if (_client.connected()) _client.disconnect();
}

bool MqttClientMgr::connectIfNeeded() {
  if (_client.connected()) return true;
  if (_nextRetryAt && millis() < _nextRetryAt) return false;
  if (millis() - _lastReconnectAttempt < 5000) return false;
  _lastReconnectAttempt = millis();

  logLine(String("[MQTT] connecting to ") + _server + ":" + String(_port) + "...");
  bool ok = _client.connect(
      _clientId.c_str(),
      _user.length() ? _user.c_str() : nullptr,
      _pass.length() ? _pass.c_str() : nullptr,
      _availabilityTopic.c_str(),
      0,
      true,
      "offline");
  if (!ok) {
    logLine(String("[MQTT] connect failed, rc=") + String(_client.state()));
    if (_failCount < 255) _failCount++;
    if (_failCount >= 3) {
      _nextRetryAt = millis() + 60000UL;
      logLine(String(F("[MQTT] backoff 60s after repeated failures")));
    }
    return false;
  }

  _failCount = 0;
  _nextRetryAt = 0;
  _client.publish(_availabilityTopic.c_str(), "online", true);
  logLine(String(F("[MQTT] connected, availability=online")));
  return true;
}

void MqttClientMgr::publishDiscovery() {
  if (!_client.connected()) return;

  const String deviceId = String("pd_logger_") + chipIdHex();
  const String deviceName = String("PD-Logger ") + chipIdHex();

  auto publishSensor = [&](const char* suffix,
                           const char* name,
                           const char* deviceClass,
                           const char* unit,
                           const char* valueTemplate) {
    StaticJsonDocument<512> doc;
    doc["name"] = name;
    doc["uniq_id"] = deviceId + "_" + suffix;
    doc["stat_t"] = _baseTopic + "/state";
    doc["avty_t"] = _availabilityTopic;
    doc["pl_avail"] = "online";
    doc["pl_not_avail"] = "offline";
    doc["dev_cla"] = deviceClass;
    doc["unit_of_meas"] = unit;
    doc["val_tpl"] = valueTemplate;

    JsonObject dev = doc.createNestedObject("device");
    JsonArray ids = dev.createNestedArray("identifiers");
    ids.add(deviceId);
    dev["name"] = deviceName;
    dev["model"] = "PD-Logger";
    dev["manufacturer"] = "ESP8266";

    String payload;
    serializeJson(doc, payload);
    const String topic = String("homeassistant/sensor/") + deviceId + "_" + suffix + "/config";
    _client.publish(topic.c_str(), payload.c_str(), true);
  };

  publishSensor("voltage", "PD-Logger Voltage", "voltage", "V", "{{ value_json.voltage }}");
  publishSensor("current", "PD-Logger Current", "current", "mA", "{{ value_json.current }}");
  publishSensor("power",   "PD-Logger Power",   "power",   "W", "{{ value_json.power }}");
  logLine(String(F("[MQTT] discovery published")));
}

void MqttClientMgr::publishState() {
  if (!_client.connected() || !_latest) return;

  float v = _latest->busV;
  if (isfinite(v) && v > 60.0f) v = v / 1000.0f;
  float i = _latest->currmA;
  float p = (isfinite(v) && isfinite(i)) ? v * (i / 1000.0f) : NAN;

  StaticJsonDocument<128> doc;
  if (isfinite(v)) doc["voltage"] = v;
  if (isfinite(i)) doc["current"] = i;
  if (isfinite(p)) doc["power"] = p;

  String payload;
  serializeJson(doc, payload);
  const String topic = _baseTopic + "/state";
  _client.publish(topic.c_str(), payload.c_str(), true);
  logLine(String("[MQTT] state published: ") + payload);
}

String MqttClientMgr::chipIdHex() const {
  char buf[9];
  snprintf(buf, sizeof(buf), "%06X", ESP.getChipId());
  return String(buf);
}
