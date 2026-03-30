/*
 * DMX Remapper ESP32 — hybrid approach
 * --------------------------------------
 * TX : esp_dmx sur DMX_NUM_1 (UART1) — fiable, break correct
 * RX: HardwareSerial on UART2     — évite le bug DMX_NUM_2
 *
 * Required versions :
 *   - Core Arduino ESP32 : 2.0.17  (Espressif)
 *   - esp_dmx            : 4.1.0   (someweisguy)
 *   - ArduinoJson        : 7.x
 *   - ESPAsyncWebServer  : 3.x (ESP32Async)
 *   - AsyncTCP           : 3.x (ESP32Async)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_dmx.h>
#include "web_ui.h"

// ─── Pins ────────────────────────────────────────────────────────────────────
// TX sur UART1 (esp_dmx)
#define DMX_TX_PIN      17
#define DMX_TX_EN_PIN    5

// RX sur UART2 (HardwareSerial natif)
#define DMX_RX_PIN      16
#define DMX_RX_EN_PIN    4

#define LED_RX_PIN       2
#define LED_TX_PIN      15

// ─── DMX TX — esp_dmx on DMX_NUM_1 ─────────────────────────────────────────
static dmx_port_t txPort = DMX_NUM_1;

// ─── DMX RX — HardwareSerial on UART2 ──────────────────────────────────────
#define DMX_BAUD    250000
#define DMX_SERIAL  SERIAL_8N2
#define DMX_BUF_SZ  600

static int      rxPos    = -1;
static uint32_t lastRxUs = 0;
static bool     newPacket = false;

// ─── Buffers DMX ─────────────────────────────────────────────────────────────
uint8_t dmxIn[DMX_PACKET_SIZE]  = {0};
uint8_t dmxOut[DMX_PACKET_SIZE] = {0};

// ─── Holdover ────────────────────────────────────────────────────────────────
#define DMX_HOLDOVER_MS  500
static uint32_t      lastFrameAt = 0;
static uint32_t      lastTxAt    = 0;
static volatile bool forceTX     = false;

// ─── LED ─────────────────────────────────────────────────────────────────────
#define LED_TOGGLE_MS  150
#define LED_TIMEOUT_MS 500
static uint32_t lastToggleAt = 0;
static bool     ledState     = false;

// ─── LED TX ──────────────────────────────────────────────────────────────────
#define LED_TX_FLASH_MS  30        // flash duration per sent frame
static uint32_t ledTxOffAt = 0;
const char* AP_SSID = "DMXR";
const char* AP_PASS = "dmx12345";

// ─── Web + NVS ───────────────────────────────────────────────────────────────
AsyncWebServer server(80);
Preferences    prefs;

// ─── Mappings ────────────────────────────────────────────────────────────────
struct MappingEntry {
  int    srcAddr;
  int    channels;
  String label;
  std::vector<int> destAddrs;
};

portMUX_TYPE mappingsMux = portMUX_INITIALIZER_UNLOCKED;
std::vector<MappingEntry> mappings;
std::vector<MappingEntry> pendingMappings;
volatile bool mappingsPending = false;
volatile bool saveNeeded      = false;
volatile bool bypassMode      = false;

portMUX_TYPE dmxMux = portMUX_INITIALIZER_UNLOCKED;

// ═════════════════════════════════════════════════════════════════════════════
//  DMX TX — via esp_dmx (UART1)
// ═════════════════════════════════════════════════════════════════════════════
void sendDMX() {
  digitalWrite(LED_TX_PIN, HIGH);
  ledTxOffAt = millis() + LED_TX_FLASH_MS;
  dmx_write(txPort, dmxOut, DMX_PACKET_SIZE);
  dmx_send_num(txPort, DMX_PACKET_SIZE);
  dmx_wait_sent(txPort, DMX_TIMEOUT_TICK);
}

// ═════════════════════════════════════════════════════════════════════════════
//  DMX RX — HardwareSerial state machine (UART2)
//  Silence > 1000 µs = break → new frame
// ═════════════════════════════════════════════════════════════════════════════
void readDMX() {
  uint32_t now = micros();

  if (rxPos >= 0 && (now - lastRxUs) > 1000) {
    if (rxPos > 1) {
      newPacket   = true;
      lastFrameAt = millis();
    }
    rxPos = -1;
  }

  while (Serial2.available()) {
    uint8_t b = (uint8_t)Serial2.read();
    lastRxUs  = micros();

    if (rxPos == -1) {
      if (b == 0x00) { rxPos = 0; dmxIn[0] = 0; }
    } else {
      rxPos++;
      if (rxPos < DMX_PACKET_SIZE) dmxIn[rxPos] = b;
      if (rxPos >= DMX_PACKET_SIZE - 1) {
        newPacket   = true;
        lastFrameAt = millis();
        rxPos       = -1;
      }
    }
  }
}

// ─── Remapping ───────────────────────────────────────────────────────────────
void applyMappings() {
  memcpy(dmxOut, dmxIn, DMX_PACKET_SIZE);
  for (auto& m : mappings) {
    if (m.srcAddr < 1 || m.srcAddr > 512) continue;
    for (int dest : m.destAddrs) {
      if (dest == m.srcAddr) continue;
      for (int c = 0; c < m.channels; c++) {
        int si = m.srcAddr + c, di = dest + c;
        if (si >= DMX_PACKET_SIZE || di >= DMX_PACKET_SIZE) break;
        dmxOut[di] = dmxIn[si];
      }
    }
  }
}

// ─── JSON ────────────────────────────────────────────────────────────────────
String serializeMappings() {
  JsonDocument doc;
  JsonArray arr = doc["mappings"].to<JsonArray>();
  for (auto& m : mappings) {
    JsonObject obj = arr.add<JsonObject>();
    obj["src"]      = m.srcAddr;
    obj["channels"] = m.channels;
    obj["label"]    = m.label;
    JsonArray dests = obj["dests"].to<JsonArray>();
    for (int d : m.destAddrs) dests.add(d);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// ─── NVS ─────────────────────────────────────────────────────────────────────
void doSaveMappings() {
  String json = serializeMappings();
  prefs.begin("dmx-remap", false);
  prefs.putString("mappings", json);
  prefs.end();
  Serial.println("[NVS] Save OK.");
}

void loadMappings() {
  prefs.begin("dmx-remap", true);
  String json = prefs.getString("mappings", "");
  prefs.end();
  if (json.isEmpty()) {
    mappings.push_back({ 1, 8, "Groupe A", {1, 9, 17, 25} });
    mappings.push_back({ 50, 8, "Groupe B", {50, 58, 66} });
    Serial.println("[NVS] Default values.");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, json)) { Serial.println("[NVS] JSON error!"); return; }
  mappings.clear();
  for (JsonObject obj : doc["mappings"].as<JsonArray>()) {
    MappingEntry e;
    e.srcAddr  = obj["src"]      | 1;
    e.channels = obj["channels"] | 1;
    e.label    = obj["label"]    | "Sans nom";
    for (int d : obj["dests"].as<JsonArray>()) e.destAddrs.push_back(d);
    mappings.push_back(e);
  }
  Serial.printf("[NVS] %d rule(s).\n", mappings.size());
}

// ─── Web routes ──────────────────────────────────────────────────────────────
void setupRoutes() {

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/html", getWebUI());
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    String json;
    portENTER_CRITICAL(&mappingsMux);
    json = serializeMappings();
    portEXIT_CRITICAL(&mappingsMux);
    json.remove(json.length() - 1);
    json += ",\"bypass\":";
    json += bypassMode ? "true}" : "false}";
    r->send(200, "application/json", json);
  });

  AsyncCallbackJsonWebHandler* saveHandler =
    new AsyncCallbackJsonWebHandler("/api/config",
      [](AsyncWebServerRequest* r, JsonVariant& json) {
        if (!json.is<JsonObject>()) { r->send(400); return; }
        portENTER_CRITICAL(&mappingsMux);
        pendingMappings.clear();
        for (JsonObject obj : json["mappings"].as<JsonArray>()) {
          MappingEntry e;
          e.srcAddr  = obj["src"]      | 1;
          e.channels = obj["channels"] | 1;
          e.label    = obj["label"]    | "Sans nom";
          for (int d : obj["dests"].as<JsonArray>()) e.destAddrs.push_back(d);
          pendingMappings.push_back(e);
        }
        mappingsPending = true;
        saveNeeded      = true;
        portEXIT_CRITICAL(&mappingsMux);
        r->send(200, "application/json", "{\"status\":\"ok\"}");
      });
  server.addHandler(saveHandler);

  server.on("/api/bypass", HTTP_POST, [](AsyncWebServerRequest* r) {
    bypassMode = !bypassMode;
    r->send(200, "application/json",
            bypassMode ? "{\"bypass\":true}" : "{\"bypass\":false}");
  });

  server.on("/api/dmx", HTTP_GET, [](AsyncWebServerRequest* r) {
    static uint8_t snapIn[DMX_PACKET_SIZE];
    static uint8_t snapOut[DMX_PACKET_SIZE];
    portENTER_CRITICAL(&dmxMux);
    memcpy(snapIn,  dmxIn,  DMX_PACKET_SIZE);
    memcpy(snapOut, dmxOut, DMX_PACKET_SIZE);
    portEXIT_CRITICAL(&dmxMux);
    String s;
    s.reserve(4300);
    s = "{\"in\":[";
    for (int i = 1; i < DMX_PACKET_SIZE; i++) { s += snapIn[i];  if (i < DMX_PACKET_SIZE-1) s += ','; }
    s += "],\"out\":[";
    for (int i = 1; i < DMX_PACKET_SIZE; i++) { s += snapOut[i]; if (i < DMX_PACKET_SIZE-1) s += ','; }
    s += "]}";
    r->send(200, "application/json", s);
  });

  server.on("/api/test/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    portENTER_CRITICAL(&dmxMux);
    memcpy(dmxOut, dmxIn, DMX_PACKET_SIZE);
    portEXIT_CRITICAL(&dmxMux);
    forceTX = true;
    r->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/test/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    portENTER_CRITICAL(&dmxMux);
    if (bypassMode) memcpy(dmxOut, dmxIn, DMX_PACKET_SIZE);
    else            applyMappings();
    portEXIT_CRITICAL(&dmxMux);
    forceTX = true;
    r->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/test", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("ch", true) || !r->hasParam("val", true)) {
      r->send(400, "application/json", "{\"error\":\"missing params\"}");
      return;
    }
    int ch  = r->getParam("ch",  true)->value().toInt();
    int val = r->getParam("val", true)->value().toInt();
    if (ch < 1 || ch > 512 || val < 0 || val > 255) {
      r->send(400, "application/json", "{\"error\":\"value out of range\"}");
      return;
    }
    portENTER_CRITICAL(&dmxMux);
    dmxOut[ch] = (uint8_t)val;
    portEXIT_CRITICAL(&dmxMux);
    forceTX = true;
    r->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  server.on("/api/ping", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", "{\"status\":\"rebooting\"}");
    delay(300); ESP.restart();
  });

  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    prefs.begin("dmx-remap", false); prefs.clear(); prefs.end();
    r->send(200, "application/json", "{\"status\":\"reset\"}");
    delay(300); ESP.restart();
  });

  server.onNotFound([](AsyncWebServerRequest* r) { r->send(404); });
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_RX_PIN,    OUTPUT);
  pinMode(LED_TX_PIN,    OUTPUT);
  pinMode(DMX_RX_EN_PIN, OUTPUT);
  digitalWrite(LED_RX_PIN,    LOW);
  digitalWrite(LED_TX_PIN,    LOW);
  digitalWrite(DMX_RX_EN_PIN, LOW);  // MAX485 RX : permanent listen

  // ── RX: HardwareSerial on UART2 ──
  Serial2.setRxBufferSize(DMX_BUF_SZ);
  Serial2.begin(DMX_BAUD, DMX_SERIAL, DMX_RX_PIN, -1);
  Serial.println("[DMX] RX (UART2 HardwareSerial) OK.");

  // ── TX: esp_dmx on DMX_NUM_1 ──
  // Only DMX_NUM_1 is stable with esp_dmx 4.1.0 on ESP32-WROOM
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_personality_t personalities[] = {};
  int personality_count = 0;
  dmx_driver_install(txPort, &config, personalities, personality_count);
  dmx_set_pin(txPort, DMX_TX_PIN, -1, DMX_TX_EN_PIN);
  Serial.println("[DMX] TX (DMX_NUM_1 esp_dmx) OK.");

  loadMappings();

  // ssid_hidden=1 : SSID not broadcast — connect manually by name
  WiFi.softAP(AP_SSID, AP_PASS, 1, 1);
  Serial.printf("[WiFi] AP: %s  IP: %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  if (MDNS.begin("dmx")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] dmx.local active");
  } else {
    Serial.println("[mDNS] Start error");
  }

  setupRoutes();
  server.begin();
  Serial.println("[HTTP] Port 80 OK");

  // Initial zero send
  sendDMX();
  lastTxAt = millis();
  Serial.println("[DMX] Initial signal sent.");
}

// ─── Loop (core 1) ───────────────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();

  // Swap mappings + NVS save
  if (mappingsPending) {
    portENTER_CRITICAL(&mappingsMux);
    mappings        = pendingMappings;
    mappingsPending = false;
    portEXIT_CRITICAL(&mappingsMux);
    Serial.printf("[MAP] %d rule(s).\n", mappings.size());
  }
  if (saveNeeded) {
    saveNeeded = false;
    doSaveMappings();
  }

  // Turn off TX LED after flash
  if (ledTxOffAt && now >= ledTxOffAt) {
    digitalWrite(LED_TX_PIN, LOW);
    ledTxOffAt = 0;
  }

  // RX activity LED
  if (now - lastFrameAt > LED_TIMEOUT_MS) {
    if (ledState) { ledState = false; digitalWrite(LED_RX_PIN, LOW); }
  } else if (now - lastToggleAt >= LED_TOGGLE_MS) {
    ledState = !ledState;
    digitalWrite(LED_RX_PIN, ledState ? HIGH : LOW);
    lastToggleAt = now;
  }

  // Read incoming DMX
  readDMX();

  if (newPacket) {
    newPacket = false;
    portENTER_CRITICAL(&dmxMux);
    if (bypassMode) memcpy(dmxOut, dmxIn, DMX_PACKET_SIZE);
    else            applyMappings();
    portEXIT_CRITICAL(&dmxMux);
    sendDMX();
    lastTxAt = now;
  } else if (forceTX || now - lastTxAt >= DMX_HOLDOVER_MS) {
    forceTX  = false;
    lastTxAt = now;
    sendDMX();
  }

  // Watchdog + WiFi breathing
  delay(1);
}