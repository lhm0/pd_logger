#include "WebServerMgr.h"
#include <time.h> 
#include <ESP8266WiFi.h>
#include "MqttClientMgr.h"

static const char* kMqttConfigPath = "/mqtt.json";

static String getParam(ESP8266WebServer& srv, const String& name) {
  if (!srv.hasArg(name)) return String();
  return srv.arg(name);
}

void WebServerMgr::serveStaticFiles() {
  // "/" explizit bedienen und _server verwenden (nicht currentServer)
  _server.on("/", HTTP_GET, [this]() {
    const char* p = "/www/index.html";
    if (!LittleFS.exists(p)) {
      _server.send(500, "text/plain", "index.html missing");
      return;
    }
    File f = LittleFS.open(p, "r");
    if (!f) {
      _server.send(500, "text/plain", "index.html open failed");
      return;
    }
    _server.streamFile(f, "text/html");
    f.close();
  });

  // explizite statische Routen
  _server.serveStatic("/index.html",    LittleFS, "/www/index.html");
  _server.serveStatic("/styles.css",    LittleFS, "/www/styles.css");
  _server.serveStatic("/app.js",        LittleFS, "/www/app.js");
  _server.serveStatic("/graphics.html", LittleFS, "/www/graphics.html");
  _server.serveStatic("/graphics.js",   LittleFS, "/www/graphics.js");
  _server.serveStatic("/graphics.css",  LittleFS, "/www/graphics.css"); 
  _server.serveStatic("/mqtt.html",     LittleFS, "/www/mqtt.html");
  _server.serveStatic("/mqtt.js",       LittleFS, "/www/mqtt.js");
}

void WebServerMgr::begin(const Measurement* latest, DataLogger* logger, MqttClientMgr* mqtt) {
  _latest = latest;
  _logger = logger;
  _mqtt = mqtt;

  // --- Statische Dateien explizit registrieren ---
  serveStaticFiles();

  // --- API-Routen ---
  _server.on("/api/health", HTTP_GET, [this]() { handleHealth(); });
  _server.on("/api/measure/latest", HTTP_GET, [this]() { handleLatest(); });
  _server.on("/api/logs", HTTP_GET, [this]() { handleLogsList(); });
  _server.on("/api/logs/download", HTTP_GET, [this]() { handleLogsDownload(); });
  _server.on("/api/logs/download_all", HTTP_GET, [this]() { handleLogsDownloadAll(); });
  _server.on("/api/logs/range", HTTP_GET, [this]() { handleLogsRange(); }); // für Grafikseite
  _server.on("/api/logs/clear", HTTP_POST, [this]() { handleLogsClear(); });
  _server.on("/api/mqtt/config", HTTP_GET, [this]() { handleMqttGet(); });
  _server.on("/api/mqtt/config", HTTP_POST, [this]() { handleMqttSave(); });
  _server.on("/api/device/info", HTTP_GET, [this]() { handleDeviceInfo(); });
  _server.on("/api/mqtt/status", HTTP_GET, [this]() { handleMqttStatus(); });

  _server.onNotFound([this]() {
    _server.send(404, "application/json", "{\"error\":\"not found\"}");
  });

  _server.begin();
}

void WebServerMgr::loop() {
  _server.handleClient();
}

void WebServerMgr::handleHealth() {
  StaticJsonDocument<128> doc;
  doc["status"] = "ok";
  String out; serializeJson(doc, out);
  _server.send(200, "application/json", out);
}

void WebServerMgr::handleLatest() {
  if (!_latest) {
    _server.send(500, "application/json", "{\"error\":\"no data\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  doc["epoch"]   = (uint32_t)_latest->epoch;
  doc["ms"]      = _latest->ms;
  doc["busV"]    = _latest->busV;
  doc["currmA"]  = _latest->currmA;
  doc["powermW"] = _latest->powermW;
  doc["shuntmV"] = _latest->shuntmV;
  doc["loadV"]   = _latest->loadV;
  String out; serializeJson(doc, out);
  _server.send(200, "application/json", out);
}

void WebServerMgr::handleLogsList() {
  if (!_logger) {
    _server.send(500, "application/json", "{\"error\":\"no logger\"}");
    return;
  }
  String json;
  _logger->listFilesJSON(json);
  _server.send(200, "application/json", json);
}

void WebServerMgr::handleLogsDownload() {
  if (!_logger) { _server.send(500, "text/plain", "no logger"); return; }
  String name = getParam(_server, "name");
  if (name.length() == 0) { _server.send(400, "text/plain", "Missing ?name="); return; }
  if (!name.startsWith("/logs/")) { _server.send(403, "text/plain", "forbidden"); return; }
  if (!LittleFS.exists(name)) { _server.send(404, "text/plain", "not found"); return; }
  File f = LittleFS.open(name, "r");
  if (!f) { _server.send(404, "text/plain", "not found"); return; }
  _server.sendHeader("Content-Disposition", "attachment; filename=\"" + String(f.name()) + "\"");
  _server.streamFile(f, "text/csv");
  f.close();
}

void WebServerMgr::handleLogsDownloadAll() {
  const bool debug = _server.hasArg("debug");
  if (debug) Serial.println(F("[DL_ALL] DEBUG MODE"));
  else       Serial.println(F("[DL_ALL] start"));

  // --- 1) Alle /logs/log_####.csv einsammeln ---
  struct Item { int idx; String path; size_t size; };
  Item items[64];
  size_t n = 0;

  auto parseIndexFromBase = [](const String& base, int& outIdx) -> bool {
    // erwartet "log_0003.csv" oder allgemein "<prefix>_<####>.csv"
    int us  = base.indexOf('_');
    int dot = base.lastIndexOf('.');
    if (us < 0 || dot < 0 || dot <= us + 1) return false;
    String idxStr = base.substring(us + 1, dot);
    if (idxStr.length() == 0) return false;
    for (unsigned i = 0; i < idxStr.length(); ++i)
      if (idxStr[i] < '0' || idxStr[i] > '9') return false;
    outIdx = idxStr.toInt();
    return true;
  };

  Dir dir = LittleFS.openDir("/logs");
  while (dir.next()) {
    yield(); // WDT während Verzeichnislauf

    String path = dir.fileName();  // kann "/logs/log_0000.csv" oder "log_0000.csv" liefern
    // fehlenden führenden Slash korrigieren (zur Sicherheit)
    if (!path.startsWith("/")) path = "/" + path;
    // sicherstellen, dass "/logs/" drin ist:
    if (!path.startsWith("/logs/")) path = String("/logs/") + path.substring(path.lastIndexOf('/') + 1);

    int slash = path.lastIndexOf('/');
    String base = (slash >= 0) ? path.substring(slash + 1) : path;
    int idx;
    if (!parseIndexFromBase(base, idx)) {
      if (debug) Serial.printf("[DL_ALL] skip (name): %s\n", path.c_str());
      continue;
    }

    // Größe ermitteln
    size_t sz = 0;
    File f = LittleFS.open(path, "r");
    if (f) { sz = f.size(); f.close(); }

    if (n < 64) {
      items[n].idx  = idx;
      items[n].path = path;
      items[n].size = sz;
      if (debug) Serial.printf("[DL_ALL] add idx=%d path=%s size=%u\n", idx, path.c_str(), (unsigned)sz);
      n++;
    }
  }

  if (n == 0) {
    Serial.println(F("[DL_ALL] no logs found"));
    _server.send(404, "text/plain", "no logs");
    return;
  }

  // --- 2) sortieren (aufsteigend) ---
  for (size_t i = 1; i < n; ++i) {
    Item key = items[i];
    size_t j = i;
    while (j > 0 && items[j-1].idx > key.idx) {
      items[j] = items[j-1];
      j--;
    }
    items[j] = key;
  }
  if (debug) {
    for (size_t i = 0; i < n; ++i) {
      Serial.printf("[DL_ALL] order[%u]: idx=%d path=%s size=%u\n",
                    (unsigned)i, items[i].idx, items[i].path.c_str(), (unsigned)items[i].size);
    }
  }

  // --- 3) DEBUG-Text statt Download? ---
  if (debug) {
    String diag;
    diag.reserve(1024);
    diag += "DOWNLOAD ALL – DEBUG\n";
    for (size_t i = 0; i < n; ++i) {
      diag += String(i) + ": idx=" + String(items[i].idx) +
              " path=" + items[i].path +
              " size=" + String(items[i].size) + "\n";
      bool ex = LittleFS.exists(items[i].path);
      diag += "  exists=" + String(ex ? "true" : "false") + "\n";
      yield();
    }
    _server.send(200, "text/plain", diag);
    Serial.println(F("[DL_ALL] DEBUG response sent"));
    return;
  }

  // --- 4) CSV streamen (korrekte Chunked-Übertragung) ---
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Content-Disposition", "attachment; filename=\"pd_logger_all.csv\"");
  _server.sendHeader("Connection", "close"); // Safari-Freund
  _server.send(200, "text/csv", "");
  _server.sendContent("epoch;bus_V;curr_mA\n"); // Header einmal

  char buf[512];
  size_t total = 0;

  for (size_t k = 0; k < n; ++k) {
    const String& p = items[k].path;
    if (!LittleFS.exists(p)) {
      if (debug) Serial.printf("[DL_ALL] WARN not exists: %s\n", p.c_str());
      continue;
    }
    File f = LittleFS.open(p, "r");
    if (!f) {
      if (debug) Serial.printf("[DL_ALL] WARN open failed: %s\n", p.c_str());
      continue;
    }

    // Headerzeile verwerfen
    (void)f.readStringUntil('\n');

    // Rest in Chunks senden (mit korrekten Chunk-Headern)
    while (f.available()) {
      size_t r = f.readBytes(buf, sizeof(buf));
      if (r) {
        // sendContent_P sendet korrekt als Chunk (ohne String-Kopie)
        _server.sendContent_P(buf, r);
        total += r;
      }
      yield(); // WDT füttern
    }
    f.close();
  }

  // finaler leerer Chunk -> beendet die Antwort sauber
  _server.sendContent("");

  Serial.printf("[DL_ALL] done, streamed %u bytes (payload)\n", (unsigned)total);
}

void WebServerMgr::handleLogsRange() {
  const bool debug = _server.hasArg("debug");
  String secArg = _server.hasArg("sec") ? _server.arg("sec") : String("max");

  // 1) Zeitfenster bestimmen
  time_t nowEpoch = time(nullptr);
  if (nowEpoch < 100000) nowEpoch = 0; // falls NTP noch nicht synchron

  long windowSec = 0; // 0 => alles
  if (!secArg.equalsIgnoreCase("max")) {
    windowSec = secArg.toInt(); // ungültig -> 0
    if (windowSec < 0) windowSec = 0;
  }
  time_t minEpoch = (nowEpoch > 0 && windowSec > 0) ? (nowEpoch - windowSec) : 0;

  if (debug) {
    Serial.printf("[RANGE] secArg=%s now=%ld min=%ld\n",
                  secArg.c_str(), (long)nowEpoch, (long)minEpoch);
  }

  // 2) Log-Dateien einsammeln & sortieren (wie bei Download_All)
  struct Item { int idx; String path; };
  Item items[64];
  size_t n = 0;

  auto parseIndexFromBase = [](const String& base, int& outIdx) -> bool {
    int us  = base.indexOf('_');
    int dot = base.lastIndexOf('.');
    if (us < 0 || dot < 0 || dot <= us + 1) return false;
    String idxStr = base.substring(us + 1, dot);
    if (idxStr.length() == 0) return false;
    for (unsigned i = 0; i < idxStr.length(); ++i)
      if (idxStr[i] < '0' || idxStr[i] > '9') return false;
    outIdx = idxStr.toInt();
    return true;
  };

  Dir dir = LittleFS.openDir("/logs");
  while (dir.next()) {
    yield();
    String path = dir.fileName();
    if (!path.startsWith("/")) path = "/" + path;
    if (!path.startsWith("/logs/")) {
      int slash = path.lastIndexOf('/');
      path = String("/logs/") + (slash >= 0 ? path.substring(slash + 1) : path);
    }
    int slash = path.lastIndexOf('/');
    String base = (slash >= 0) ? path.substring(slash + 1) : path;

    int idx;
    if (!parseIndexFromBase(base, idx)) continue;
    if (n < 64) { items[n].idx = idx; items[n].path = path; n++; }
  }

  if (n == 0) {
    _server.send(404, "text/plain", "no logs");
    return;
  }

  for (size_t i = 1; i < n; ++i) {
    Item key = items[i];
    size_t j = i;
    while (j > 0 && items[j-1].idx > key.idx) { items[j] = items[j-1]; j--; }
    items[j] = key;
  }

  if (debug) {
    Serial.printf("[RANGE] %u files\n", (unsigned)n);
    for (size_t i = 0; i < n; ++i) Serial.printf("  [%u] %s\n", (unsigned)i, items[i].path.c_str());
  }

  // 3) CSV streamen (nur Zeilen >= minEpoch), korrekter Chunked-Ende
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Content-Type", "text/csv; charset=utf-8");
  _server.sendHeader("Cache-Control", "no-store");
  _server.sendHeader("Connection", "close");
  _server.send(200, "text/csv", "");
  _server.sendContent("epoch;bus_V;curr_mA\n");

  size_t outCount = 0;
  for (size_t k = 0; k < n; ++k) {
    File f = LittleFS.open(items[k].path, "r");
    if (!f) continue;

    // Header der Datei weg
    (void)f.readStringUntil('\n');

    while (f.available()) {
      String line = f.readStringUntil('\n'); // enthält kein '\n'
      if (line.length() == 0) continue;

      // CR am Zeilenende entfernen
      if (line.length() && line[line.length()-1] == '\r') {
        line.remove(line.length()-1);
      }

      // epoch als erstes Feld extrahieren (bis ';')
      int sc = line.indexOf(';');
      if (sc <= 0) continue; // ungültige Zeile
      String eStr = line.substring(0, sc);

      // schnelle Prüfung auf Zahl (erste Stelle)
      char c0 = eStr[0];
      if (c0 < '0' || c0 > '9') {
        // evtl. "nan" etc. -> überspringen
        continue;
      }

      // epoch vergleichen, wenn gefordert
      if (minEpoch > 0) {
        long e = eStr.toInt();
        if (e < (long)minEpoch) {
          // zu alt -> nicht senden
          continue;
        }
      }

      // Senden (+ newline)
      _server.sendContent(line + "\n");
      outCount++;
      yield();
    }
    f.close();
    yield();
  }

  // finaler leerer Chunk
  _server.sendContent("");
  if (debug) Serial.printf("[RANGE] sent %u rows\n", (unsigned)outCount);
}

void WebServerMgr::handleLogsClear() {
  if (!_logger) {
    _server.send(500, "application/json", "{\"ok\":false,\"error\":\"no logger\"}");
    return;
  }
  // Schutz: nur POST zulassen (zusätzlich zur Route-Definition)
  if (_server.method() != HTTP_POST) {
    _server.send(405, "application/json", "{\"ok\":false,\"error\":\"method not allowed\"}");
    return;
  }

  bool ok = _logger->clearAll();
  if (ok) {
    _server.send(200, "application/json", "{\"ok\":true}");
  } else {
    _server.send(500, "application/json", "{\"ok\":false,\"error\":\"clear failed\"}");
  }
}

void WebServerMgr::handleMqttGet() {
  StaticJsonDocument<256> doc;
  doc["server"] = "";
  doc["port"] = 1883;
  doc["user"] = "";
  doc["pass"] = "";

  if (LittleFS.exists(kMqttConfigPath)) {
    File f = LittleFS.open(kMqttConfigPath, "r");
    if (!f) {
      _server.send(500, "application/json", "{\"error\":\"mqtt config open failed\"}");
      return;
    }
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      doc.clear();
      doc["server"] = "";
      doc["port"] = 1883;
    }
  } else {
    File f = LittleFS.open(kMqttConfigPath, "w");
    if (f) {
      serializeJson(doc, f);
      f.close();
    }
  }

  String out;
  serializeJson(doc, out);
  _server.send(200, "application/json", out);
}

void WebServerMgr::handleMqttSave() {
  const String body = _server.arg("plain");
  if (body.length() == 0) {
    _server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty body\"}");
    return;
  }

  StaticJsonDocument<256> inDoc;
  DeserializationError err = deserializeJson(inDoc, body);
  if (err) {
    _server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
    return;
  }

  const char* server = inDoc["server"] | "";
  int port = inDoc["port"] | 0;
  const char* user = inDoc["user"] | "";
  const char* pass = inDoc["pass"] | "";
  if (port <= 0 || port > 65535) {
    _server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad port\"}");
    return;
  }

  StaticJsonDocument<256> outDoc;
  outDoc["server"] = server;
  outDoc["port"] = port;
  outDoc["user"] = user;
  outDoc["pass"] = pass;

  File f = LittleFS.open(kMqttConfigPath, "w");
  if (!f) {
    _server.send(500, "application/json", "{\"ok\":false,\"error\":\"save failed\"}");
    return;
  }
  serializeJson(outDoc, f);
  f.close();
  _server.send(200, "application/json", "{\"ok\":true}");
}

void WebServerMgr::handleDeviceInfo() {
  char buf[9];
  snprintf(buf, sizeof(buf), "%06X", ESP.getChipId());

  StaticJsonDocument<128> doc;
  doc["chipId"] = buf;
  String out;
  serializeJson(doc, out);
  _server.send(200, "application/json", out);
}

void WebServerMgr::handleMqttStatus() {
  StaticJsonDocument<256> doc;
  doc["message"] = (_mqtt ? _mqtt->lastLog() : "");
  String out;
  serializeJson(doc, out);
  _server.send(200, "application/json", out);
}
