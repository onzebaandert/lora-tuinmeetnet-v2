// =====================================================
// CLAUDE_WEMOS_DHT22_MQTT_V1
// Wemos D1 mini — DHT22 temperatuur + vochtigheid naar MQTT
//
// Hardware aansluitingen:
//   Wemos D0 (GPIO16) ──── RST          (deep sleep wakeup)
//   Wemos D2 (GPIO4)  ──── DHT22 DATA
//   Wemos 3V3         ──── DHT22 VCC
//   Wemos GND         ──── DHT22 GND
//
//   microWakeUpper VCC ──── 5V           (powerbank alive)
//   microWakeUpper GND ──── GND
//   microWakeUpper OUT ──── weerstand (47–100Ω) ──── GND
//   microWakeUpper IN  ──── microWakeUpper OUT
//
// Libraries: DHT sensor library (Adafruit), PubSubClient
// =====================================================

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include "credentials.h"

#define DHT_PIN      4        // D2
#define DHT_TYPE     DHT22

static const uint32_t SLEEP_SEC      = 60;
static const char*    MQTT_SERVER    = "192.168.2.13";
static const int      MQTT_PORT      = 1883;
static const char*    MQTT_TOPIC     = "tuin/mqtt/dht22";
static const char*    MQTT_CLIENT    = "wemos-dht22";
static const uint32_t WIFI_TIMEOUT   = 10000;
static const uint32_t MQTT_TIMEOUT   = 5000;

DHT          dht(DHT_PIN, DHT_TYPE);
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > WIFI_TIMEOUT) return false;
    delay(200);
  }
  return true;
}

bool connectMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  uint32_t t = millis();
  while (!mqttClient.connected()) {
    if (millis() - t > MQTT_TIMEOUT) return false;
    mqttClient.connect(MQTT_CLIENT);
    if (!mqttClient.connected()) delay(500);
  }
  return true;
}

void goSleep() {
  WiFi.disconnect(true);
  delay(100);
  ESP.deepSleep((uint64_t)SLEEP_SEC * 1000000ULL);
}

void setup() {
  Serial.begin(115200);
  dht.begin();

  // DHT22 heeft ~2s opwarmtijd na power-on
  delay(2000);

  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT22] leesfout — ga slapen");
    goSleep();
    return;
  }
  Serial.printf("[DHT22] %.1f°C  %.1f%%\n", temp, hum);

  if (!connectWiFi()) {
    Serial.println("[WiFi] timeout — ga slapen");
    goSleep();
    return;
  }

  if (!connectMQTT()) {
    Serial.println("[MQTT] timeout — ga slapen");
    goSleep();
    return;
  }

  char payload[64];
  snprintf(payload, sizeof(payload),
           "{\"temp\":%.1f,\"hum\":%.1f}",
           temp, hum);

  mqttClient.publish(MQTT_TOPIC, payload, false);
  Serial.printf("[MQTT] %s → %s\n", MQTT_TOPIC, payload);

  mqttClient.disconnect();
  goSleep();
}

void loop() {}
