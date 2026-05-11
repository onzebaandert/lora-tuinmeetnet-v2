// =====================================================
// CLAUDE_WEMOS_MICROWAKEUPPER_V2
// Wemos D1 mini — powerbank keepalive + DHT22 naar MQTT
//
// Hardware aansluitingen:
//   Wemos D0 (GPIO16) ──── RST          (deep sleep wakeup)
//   Wemos D2 (GPIO4)  ──── DHT22 DATA
//   Wemos 3V3         ──── DHT22 VCC
//   Wemos GND         ──── DHT22 GND
//
//   microWakeUpper VCC ──── 5V
//   microWakeUpper GND ──── GND
//   microWakeUpper OUT ──── IN
//   microWakeUpper OUT ──── weerstand (47–100Ω) ──── GND
//
// Werking keepalive:
//   Elke 25s wekt D0→RST de Wemos. De bootstroom houdt de
//   powerbank wakker. microWakeUpper geeft extra puls als backup.
//
// Werking meting:
//   Na opstarten: DHT22 lezen, WiFi + MQTT verbinden,
//   temp+hum publiceren naar tuin/mqtt/dht22, dan slapen.
//   Bij WiFi/MQTT timeout: direct slapen (keepalive werkt altijd).
//
// Libraries: DHT sensor library (Adafruit), PubSubClient
// =====================================================

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include "credentials.h"

#define DHT_PIN      4        // D2
#define DHT_TYPE     DHT22

static const uint32_t SLEEP_SEC    = 25;
static const char*    MQTT_SERVER  = "192.168.2.13";
static const int      MQTT_PORT    = 1883;
static const char*    MQTT_TOPIC   = "tuin/mqtt/dht22";
static const char*    MQTT_CLIENT  = "wemos-keepalive-dht22";
static const uint32_t WIFI_TIMEOUT = 10000;
static const uint32_t MQTT_TIMEOUT = 5000;

DHT          dht(DHT_PIN, DHT_TYPE);
WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

void goSleep() {
  WiFi.disconnect(true);
  delay(100);
  ESP.deepSleep((uint64_t)SLEEP_SEC * 1000000ULL);
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
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

void setup() {
  Serial.begin(115200);
  dht.begin();
  delay(2000);  // DHT22 opwarmtijd

  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();

  if (isnan(temp) || isnan(hum)) {
    Serial.println("[DHT22] leesfout");
    goSleep();
    return;
  }
  Serial.printf("[DHT22] %.1f C  %.1f%%\n", temp, hum);

  if (!connectWiFi()) {
    Serial.println("[WiFi] timeout");
    goSleep();
    return;
  }

  if (!connectMQTT()) {
    Serial.println("[MQTT] timeout");
    goSleep();
    return;
  }

  char payload[64];
  snprintf(payload, sizeof(payload), "{\"temp\":%.1f,\"hum\":%.1f}", temp, hum);
  mqttClient.publish(MQTT_TOPIC, payload, false);
  Serial.printf("[MQTT] %s → %s\n", MQTT_TOPIC, payload);

  mqttClient.disconnect();
  goSleep();
}

void loop() {}
