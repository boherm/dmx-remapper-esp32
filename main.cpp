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
// Per-channel test state — only tested channels are frozen in dmxOut
static uint8_t testVals[DMX_PACKET_SIZE]   = {0};     // test values per channel
static bool    testActive[DMX_PACKET_SIZE] = {false}; // which channels are being tested

bool hasTestActive() {
  for (int i = 1; i < DMX_PACKET_SIZE; i++) if (testActive[i]) return true;
  return false;
}

// ─── Input state ──────────────────────────────────────────────────────────────
// gracePending > 0 : ignore input frames (grace after first connect / reconnect)
#define RECONNECT_STABLE_FRAMES 5
static int  gracePending      = 0;
static bool inputEverReceived = false;
static bool hasValidOutput    = false; // true after first grace completes — gates holdover

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
// Logic:
//   1. All channels pass through by default (memcpy)
//   2. Source channels of a mapping are zeroed out — they no longer appear
//      at their original position in the output
//   3. Source values are copied to each destination (overwrites whatever
//      was there, including pass-through values from another group)
void applyMappings() {
  memcpy(dmxOut, dmxIn, DMX_PACKET_SIZE);

  // Pass 1: zero out all source channels that have at least one destination
  for (auto& m : mappings) {
    if (m.srcAddr < 1 || m.srcAddr > 512) continue;
    if (m.destAddrs.empty()) continue;
    for (int c = 0; c < m.channels; c++) {
      int si = m.srcAddr + c;
      if (si < DMX_PACKET_SIZE) dmxOut[si] = 0;
    }
  }

  // Pass 2: copy source values to all destinations
  for (auto& m : mappings) {
    if (m.srcAddr < 1 || m.srcAddr > 512) continue;
    for (int dest : m.destAddrs) {
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
  static bool    snapTest[DMX_PACKET_SIZE];
  portENTER_CRITICAL(&dmxMux);
  memcpy(snapIn,   dmxIn,      DMX_PACKET_SIZE);
  memcpy(snapOut,  dmxOut,     DMX_PACKET_SIZE);
  memcpy(snapTest, testActive, DMX_PACKET_SIZE);
  portEXIT_CRITICAL(&dmxMux);

  String s;
  s.reserve(4400);
  s = "{\"in\":[";
  for (int i = 1; i < DMX_PACKET_SIZE; i++) { s += snapIn[i];  if (i < DMX_PACKET_SIZE-1) s += ','; }
  s += "],\"out\":[";
  for (int i = 1; i < DMX_PACKET_SIZE; i++) { s += snapOut[i]; if (i < DMX_PACKET_SIZE-1) s += ','; }
  // Compact list of tested channel indices (1-based)
  s += "],\"test\":[";
  bool first = true;
  for (int i = 1; i < DMX_PACKET_SIZE; i++) {
    if (snapTest[i]) { if (!first) s += ','; s += i; first = false; }
  }
  // Input connected = signal received recently
  bool inputConnected = (millis() - lastFrameAt) < DMX_HOLDOVER_MS;
  s += inputConnected ? "],\"input\":true}" : "],\"input\":false}";
  events.send(s.c_str(), "dmx", millis());
  lastSseAt = millis();
}

// ─── Favicon ─────────────────────────────────────────────────────────────────
static const char FAVICON_SVG[] PROGMEM =
  "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\">"
  "<rect width=\"36\" height=\"36\" rx=\"6\" fill=\"#ff6b1a\"/>"
  "<text x=\"18\" y=\"25\" text-anchor=\"middle\" "
  "font-family=\"ui-monospace,monospace\" font-size=\"13\" "
  "font-weight=\"bold\" fill=\"#000\">DMX</text>"
  "</svg>";


void setupRoutes() {

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/html", getWebUI());
  });

  server.on("/favicon.svg", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send_P(200, "image/svg+xml", FAVICON_SVG);
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
    portENTER_CRITICAL(&dmxMux);
    memset(testActive, false, sizeof(testActive));
    memset(testVals,   0,     sizeof(testVals));
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
    testVals[ch]   = (uint8_t)val;
    testActive[ch] = true;
    dmxOut[ch]     = (uint8_t)val;
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

  // Do not send DMX at boot — wait for first valid input frames (grace period)
  // This avoids briefly zeroing fixtures on ESP32 restart
  lastTxAt = millis();
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
  // Live mode : fast blink ~6 Hz when signal present, off if no signal
  // Test mode : slow double-blink pattern regardless of signal
  if (hasTestActive()) {
    // Double-blink: ON 80ms, OFF 80ms, ON 80ms, OFF 560ms (period = 800ms)
    uint32_t phase = now % 800;
    bool on = (phase < 80) || (phase >= 160 && phase < 240);
    if (on != ledState) {
      ledState = on;
      digitalWrite(LED_RX_PIN, on ? HIGH : LOW);
    }
  } else if (now - lastFrameAt > LED_TIMEOUT_MS) {
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

      if (!inputEverReceived) {
        // First frame ever — start grace, do not apply yet
        inputEverReceived = true;
        gracePending = RECONNECT_STABLE_FRAMES - 1; // this frame counts as 1
      } else if (gracePending > 0) {
        gracePending--;
      }

      if (gracePending == 0) {
        // Grace over — apply input and send
        applyMappings();
        for (int i = 1; i < DMX_PACKET_SIZE; i++)
          if (testActive[i]) dmxOut[i] = testVals[i];
        hasValidOutput = true;
        portEXIT_CRITICAL(&dmxMux);
        lastTxAt = now;
        sendDMX();
      } else {
        // Still in grace — holdover keeps sending last good values
        portEXIT_CRITICAL(&dmxMux);
      }
      lastFrameAt = now;
    }
  }

  // Signal loss → arm grace for next reconnection
  if (inputEverReceived && gracePending == 0 && now - lastFrameAt > DMX_HOLDOVER_MS) {
    gracePending = RECONNECT_STABLE_FRAMES;
  }

  // Holdover:
  // - forceTX (test) always fires regardless of input state
  // - automatic holdover only fires after first grace completes (hasValidOutput)
  //   so zeros are never sent at boot or on first connection
  // - during reconnect grace, hasValidOutput is already true so holdover
  //   correctly sends last good dmxOut values
  if (forceTX || (hasValidOutput && now - lastTxAt >= DMX_HOLDOVER_MS)) {
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