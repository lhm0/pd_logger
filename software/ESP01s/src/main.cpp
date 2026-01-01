#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>   // mDNS / Bonjour
#include <Wire.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>       

#include "Config.h"
#include "Measurement.h"
#include "SensorINA219.h"
#include "TimeService.h"
#include "DataLogger.h"
#include "WebServerMgr.h"
#include "MqttClientMgr.h"

SensorINA219 sensor;
TimeService   timeSvc;
DataLogger    logger;
WebServerMgr  web(80);
MqttClientMgr mqtt;

Measurement latest;
unsigned long lastSample = 0;

// --- mDNS Helper ---
static bool mdnsRunning = false;

static void startMDNS() {
  MDNS.end(); // end any previous mDNS instance
  mdnsRunning = MDNS.begin("pd-logger");   // hostname "pd-logger.local"
  if (mdnsRunning) {
    MDNS.addService("http", "tcp", 80);
    Serial.println(F("mDNS ready: http://pd-logger.local"));
  } else {
    Serial.println(F("mDNS start failed"));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("Boot PD-Logger (Phase 2: Rotation)"));

  if (!LittleFS.begin()) {
    Serial.println(F("LittleFS start fehlgeschlagen!"));
  }

  // WiFi baseline
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // Stability tweaks
  WiFi.setSleepMode(WIFI_NONE_SLEEP);       // reduce timing/handshake issues
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);       // 11g often more robust than 11n
  WiFi.setOutputPower(17.0f);               // try 15–18 dBm if needed

  // Hostname for DHCP/Router & WiFiManager
  WiFi.hostname("PD-Logger");
  WiFiManager wm;
  wm.setHostname("PD-Logger");

  wm.setConnectTimeout(10);                 // 10 s connect attempt
  wm.setConfigPortalTimeout(120);           // 2 min captive portal
  wm.setBreakAfterConfig(true);             // return after successful connect

  const bool ok = wm.autoConnect("PD-Logger");
  Serial.printf("WiFi %s, IP=%s, RSSI=%d dBm\n",
                ok ? "connected" : "AP/Portal",
                WiFi.localIP().toString().c_str(),
                WiFi.RSSI());

  if (ok) {
    startMDNS();
    Serial.println(F("Reach me at: http://pd-logger.local"));
  }

  timeSvc.begin(TZ_EU_BERLIN);

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!sensor.begin(Wire)) {
    Serial.println(F("INA219 nicht gefunden – Verkabelung/Adresse prüfen!"));
  }
  sensor.setShuntCorrection(INA219_CORR);
  Wire.setClock(100000);

  if (!logger.begin(LOG_DIR, LOG_PREFIX, LOG_EXT, MAX_LOG_FILE_SIZE, MAX_LOG_FILES)) {
    Serial.println(F("Logger init fehlgeschlagen!"));
  } else {
    Serial.print(F("Aktuelle Logdatei: "));
    Serial.println(logger.currentFilePath());
  }

  web.begin(&latest, &logger, &mqtt);
  mqtt.begin(&latest);

  lastSample = millis();
}

void loop() {
  web.loop();
  mqtt.loop();

  // mDNS needs regular updates
  MDNS.update();

  // Track WiFi state changes to (re)start mDNS on connect
  static wl_status_t lastStatus = WL_IDLE_STATUS;
  wl_status_t st = WiFi.status();
  if (st != lastStatus) {
    lastStatus = st;
    if (st == WL_CONNECTED) {
      if (!mdnsRunning) startMDNS();
      Serial.printf("Connected: IP=%s\n", WiFi.localIP().toString().c_str());
    } else {
      mdnsRunning = false;
      Serial.println(F("WiFi not connected"));
    }
  }

  // Keep NAT/ARP fresh with a tiny UDP packet to the gateway every 30s
  static unsigned long lastKA = 0;
  if (st == WL_CONNECTED && millis() - lastKA >= 30000UL) {
    lastKA = millis();
    IPAddress gw = WiFi.gatewayIP();
    if ((gw[0] | gw[1] | gw[2] | gw[3]) != 0) {  // valid gateway
      WiFiUDP udp;
      if (udp.begin(0)) {                        // ephemeral source port
        udp.beginPacket(gw, 9);                  // discard/chargen-like port
        uint8_t b = 0;
        udp.write(&b, 1);                        // 1 byte is enough
        udp.endPacket();
        udp.stop();
      }
    }
    yield(); // be nice to the WDT
  }

  // Sampling & Logging
  if (millis() - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample += SAMPLE_INTERVAL_MS;
    if (sensor.read(latest)) {
      latest.epoch = timeSvc.nowEpoch();
      latest.ms    = millis();
      // compact CSV logger uses "epoch;bus_mV;curr_mA"
      logger.append(latest, String());
    } else {
      Serial.println(F("Sensor read invalid -> skipped"));
    }
  }
}
