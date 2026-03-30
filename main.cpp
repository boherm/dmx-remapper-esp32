/*
 * DMX Remapper ESP32 — hybrid approach
 * --------------------------------------
 * RX : esp_dmx on DMX_NUM_1 (UART1, GPIO16) — reliable break detection
 * TX : IDF UART  on UART_NUM_2 (GPIO17)     — avoids dmx_driver_install
 *                                              crash bug on DMX_NUM_2
 *
 * Required versions:
 *   - Arduino ESP32 core : 2.0.17 (Espressif)
 *   - esp_dmx            : 4.1.0  (someweisguy)
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
#define DMX_RX_PIN      16   // esp_dmx RX  — MAX485 RO
#define DMX_TX_PIN      17   // IDF UART TX — MAX485 DI
#define DMX_RX_EN_PIN    4   // MAX485 RX enable (LOW = listen, managed by esp_dmx)
#define DMX_TX_EN_PIN    5   // MAX485 TX enable (HIGH = emit, permanent)
#define LED_RX_PIN       2

// ─── DMX ports ───────────────────────────────────────────────────────────────
static dmx_port_t rxPort = DMX_NUM_1;   // esp_dmx RX — UART1
static dmx_port_t txPort = DMX_NUM_0;   // esp_dmx TX — UART0 (freed after Serial.end())

// ─── DMX buffers ─────────────────────────────────────────────────────────────
uint8_t dmxIn[DMX_PACKET_SIZE]  = {0};
uint8_t dmxOut[DMX_PACKET_SIZE] = {0};

// ─── Holdover ────────────────────────────────────────────────────────────────
#define DMX_HOLDOVER_MS  500
static uint32_t      lastFrameAt = 0;
static uint32_t      lastTxAt    = 0;
static volatile bool forceTX     = false;

// ─── LED RX ──────────────────────────────────────────────────────────────────
#define LED_TOGGLE_MS   80
#define LED_TIMEOUT_MS 500
static uint32_t lastToggleAt = 0;
static bool     ledState     = false;

// ─── WiFi ────────────────────────────────────────────────────────────────────
const char* AP_SSID = "DMXR";
const char* AP_PASS = "dmx12345";

// ─── Web + NVS ───────────────────────────────────────────────────────────────
AsyncWebServer  server(80);
AsyncEventSource events("/api/events");
Preferences     prefs;

#define SSE_THROTTLE_MS 70
static uint32_t lastSseAt = 0;

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
volatile bool testMode        = false;  // when true, dmxOut is not overwritten by incoming DMX

portMUX_TYPE dmxMux = portMUX_INITIALIZER_UNLOCKED;

// ═════════════════════════════════════════════════════════════════════════════
//  DMX TX — esp_dmx on DMX_NUM_0 (UART0, remapped to GPIO17)
//  UART0 is freed with Serial.end() in setup() before installing the driver.
// ═════════════════════════════════════════════════════════════════════════════
void sendDMX() {
  dmx_write(txPort, dmxOut, DMX_PACKET_SIZE);
  dmx_send_num(txPort, DMX_PACKET_SIZE);
  dmx_wait_sent(txPort, DMX_TIMEOUT_TICK);
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
    mappings.push_back({ 1, 8, "Group A", {1, 9, 17, 25} });
    mappings.push_back({ 50, 8, "Group B", {50, 58, 66} });
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
    e.label    = obj["label"]    | "Unnamed";
    for (int d : obj["dests"].as<JsonArray>()) e.destAddrs.push_back(d);
    mappings.push_back(e);
  }
  Serial.printf("[NVS] %d rule(s).\n", mappings.size());
}

// ─── SSE push — builds JSON and sends to all connected clients ───────────────
void pushSse() {
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
  events.send(s.c_str(), "dmx", millis());
  lastSseAt = millis();
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
    // Inject bypass state: {"mappings":[...]} → {"mappings":[...],"bypass":true}
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
          e.label    = obj["label"]    | "Unnamed";
          for (int d : obj["dests"].as<JsonArray>()) e.destAddrs.push_back(d);
          pendingMappings.push_back(e);
        }
        mappingsPending = true;
        saveNeeded      = true;
        portEXIT_CRITICAL(&mappingsMux);
        r->send(200, "application/json", "{\"status\":\"ok\"}");
      });
  server.addHandler(saveHandler);

  // SSE endpoint — client subscribes once, server pushes at ~14 Hz
  events.onConnect([](AsyncEventSourceClient* client) {
    client->send("connected", "info", millis());
  });
  server.addHandler(&events);

  server.on("/api/test/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    testMode = false;
    portENTER_CRITICAL(&dmxMux);
    applyMappings();
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
    testMode = true;
    forceTX  = true;
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
  pinMode(DMX_TX_EN_PIN, OUTPUT);
  digitalWrite(LED_RX_PIN,    LOW);
  digitalWrite(DMX_TX_EN_PIN, HIGH);  // MAX485 TX: always emit

  // ── RX: esp_dmx on DMX_NUM_1 (UART1, GPIO16) ──
  dmx_config_t rxConfig = DMX_CONFIG_DEFAULT;
  dmx_personality_t personalities[] = {};
  dmx_driver_install(rxPort, &rxConfig, personalities, 0);
  dmx_set_pin(rxPort, -1, DMX_RX_PIN, DMX_RX_EN_PIN);
  Serial.println("[DMX] RX (esp_dmx DMX_NUM_1) OK.");

  // ── TX: esp_dmx on DMX_NUM_0 (UART0, remapped to GPIO17) ──
  // UART0 is normally used by Serial (USB debug). We free it here so
  // esp_dmx can use it for TX. Serial.println() calls after this are silent.
  Serial.println("[DMX] Freeing UART0 for TX...");
  Serial.flush();
  Serial.end();  // release UART0 driver
  dmx_config_t txConfig = DMX_CONFIG_DEFAULT;
  dmx_driver_install(txPort, &txConfig, personalities, 0);
  dmx_set_pin(txPort, DMX_TX_PIN, -1, DMX_TX_EN_PIN);

  loadMappings();

  // ssid_hidden=1: SSID not broadcast — connect manually by name
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

  // Swap mappings + NVS save delegated to core 1
  if (mappingsPending) {
    portENTER_CRITICAL(&mappingsMux);
    mappings        = pendingMappings;
    mappingsPending = false;
    portEXIT_CRITICAL(&mappingsMux);
    Serial.printf("[MAP] %d rule(s) applied.\n", mappings.size());
  }
  if (saveNeeded) {
    saveNeeded = false;
    doSaveMappings();
  }

  // RX activity LED
  if (now - lastFrameAt > LED_TIMEOUT_MS) {
    if (ledState) { ledState = false; digitalWrite(LED_RX_PIN, LOW); }
  } else if (now - lastToggleAt >= LED_TOGGLE_MS) {
    ledState = !ledState;
    digitalWrite(LED_RX_PIN, ledState ? HIGH : LOW);
    lastToggleAt = now;
  }

  // ── DMX reception via esp_dmx (non-blocking, timeout=0) ──
  dmx_packet_t packet;
  if (dmx_receive(rxPort, &packet, 0)) {
    if (packet.err == DMX_OK) {
      portENTER_CRITICAL(&dmxMux);
      dmx_read(rxPort, dmxIn, DMX_PACKET_SIZE);
      if (!testMode) {
        applyMappings();
        lastTxAt = now;
        portEXIT_CRITICAL(&dmxMux);
        sendDMX();
      } else {
        portEXIT_CRITICAL(&dmxMux);
      }
      lastFrameAt = now;
    }
  }

  // Holdover + immediate test send
  if (forceTX || now - lastTxAt >= DMX_HOLDOVER_MS) {
    forceTX  = false;
    lastTxAt = now;
    sendDMX();
  }

  // SSE push — throttled to SSE_THROTTLE_MS, only if clients connected
  if (events.count() > 0 && now - lastSseAt >= SSE_THROTTLE_MS) {
    pushSse();
  }

  // Yield to idle task — feeds watchdog and lets WiFi/TCP breathe
  delay(1);
}