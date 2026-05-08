// =====================================================
// CLAUDE_WEMOS_MICROWAKEUPPER_V1
// Wemos D1 mini — MQTT keepalive node
// Powerbank alive houden via microWakeUpper
//
// Hardware aansluitingen:
//   Wemos D0 (GPIO16) ──── RST          (deep sleep wakeup)
//   microWakeUpper VCC ──── 5V
//   microWakeUpper GND ──── GND
//   microWakeUpper OUT ──── weerstand (47–100Ω) ──── GND
//   microWakeUpper IN  ──── microWakeUpper OUT  (of los laten)
//
// Werking:
//   1. Wemos deep sleep (D0→RST), microWakeUpper trekt
//      periodiek stroom via weerstand → powerbank blijft aan
//   2. Na SLEEP_SEC seconden: eigen wakeup, WiFi+MQTT connect
//   3. Publiceert status / uptime keepalive
//   4. Terug naar deep sleep
// =====================================================

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "credentials.h"

// ---------- SLEEP INTERVAL ----------
static const uint32_t SLEEP_SEC = 60;   // seconden tussen MQTT publishes

// ---------- MQTT ----------
static const char* MQTT_SERVER  = "192.168.2.13";
static const int   MQTT_PORT    = 1883;
static const char* MQTT_TOPIC   = "tuin/wemos/keepalive";
static const char* MQTT_CLIENT  = "wemos-keepalive";
static const bool  MQTT_RETAIN  = false;

// ---------- TIMEOUTS ----------
static const uint32_t WIFI_TIMEOUT_MS = 10000;
static const uint32_t MQTT_TIMEOUT_MS =  5000;

// RTC memory: teller bewaard tijdens deep sleep
RTC_DATA_ATTR static uint32_t bootCount = 0;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// --------------------------------------------------

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > WIFI_TIMEOUT_MS) return false;
    delay(200);
  }
  return true;
}

bool connectMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  uint32_t t = millis();
  while (!mqttClient.connected()) {
    if (millis() - t > MQTT_TIMEOUT_MS) return false;
    mqttClient.connect(MQTT_CLIENT);
    if (!mqttClient.connected()) delay(500);
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  bootCount++;

  Serial.printf("\n[BOOT #%u] Wemos keepalive node\n", bootCount);

  if (!connectWiFi()) {
    Serial.println("[WiFi] timeout — ga slapen");
    ESP.deepSleep((uint64_t)SLEEP_SEC * 1000000ULL);
    return;
  }
  Serial.printf("[WiFi] verbonden: %s\n", WiFi.localIP().toString().c_str());

  if (!connectMQTT()) {
    Serial.println("[MQTT] timeout — ga slapen");
    WiFi.disconnect(true);
    ESP.deepSleep((uint64_t)SLEEP_SEC * 1000000ULL);
    return;
  }
  Serial.println("[MQTT] verbonden");

  // Bouw payload
  char payload[64];
  snprintf(payload, sizeof(payload),
           "{\"boot\":%u,\"rssi\":%d,\"ip\":\"%s\"}",
           bootCount,
           WiFi.RSSI(),
           WiFi.localIP().toString().c_str());

  bool ok = mqttClient.publish(MQTT_TOPIC, payload, MQTT_RETAIN);
  Serial.printf("[MQTT] publish %s: %s\n", ok ? "OK" : "FAILED", payload);

  mqttClient.disconnect();
  WiFi.disconnect(true);
  delay(100);

  Serial.printf("[SLEEP] %u seconden\n", SLEEP_SEC);
  ESP.deepSleep((uint64_t)SLEEP_SEC * 1000000ULL);
}

void loop() {
  // niet bereikt — deep sleep herstart altijd via setup()
}
