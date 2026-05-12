#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "credentials.h"

// =====================================================
// LORA_VV_RECEIVER_MQTT_HELTEC_V2
// Heltec LoRa 32 V3 — niet-blokkerende ontvangst
//
// Verschil met V1:
//   - radio.receive() vervangen door interrupt-gebaseerde
//     ontvangst via setDio1Action() + startReceive()
//   - loop() blokkeert nooit → WiFi/MQTT blijft altijd alive
//   - OLED toont uptime, RX teller, RSSI/SNR
//
// Nieuw in V2:
//   - Periodieke statuspublicatie naar tuin/receiver/status
//     elke 30s: uptime, rx_total, rx_mqtt_ok, wifi_rssi
//     zodat Grafana receiver-activiteit toont ook zonder LoRa data
// =====================================================

// ---------- MQTT ----------
const char* mqtt_server       = "192.168.2.13";
const int   mqtt_port         = 1883;
const char* mqtt_topic        = "tuin/lora/test";
const char* mqtt_status_topic = "tuin/receiver/status";
const bool  MQTT_RETAIN       = false;

// ---------- HELTEC LORA 32 V3 — SX1262 PINS ----------
static const int PIN_NSS  = 8;
static const int PIN_DIO1 = 14;
static const int PIN_NRST = 12;
static const int PIN_BUSY = 13;
static const int PIN_SCK  = 9;
static const int PIN_MISO = 11;
static const int PIN_MOSI = 10;

// ---------- OLED PINS ----------
static const int OLED_SDA = 17;
static const int OLED_SCL = 18;
static const int OLED_RST = 21;

// ---------- LORA SETTINGS ----------
static const float   LORA_FREQ = 868.0;
static const float   LORA_BW   = 125.0;
static const uint8_t LORA_SF   = 11;
static const uint8_t LORA_CR   = 5;
static const uint8_t LORA_SYNC = 0x12;
static const int8_t  LORA_PWR  = 14;

// ---------- TBAT SPANNING METING ----------
#define TBAT_PIN        4
#define VBAT_SAMPLES    8
static const float TBAT_RATIO = 5.0f;   // sensor verhouding 5:1 (0-25V module)
static const float TBAT_CAL   = 1.0f;   // kalibratie factor (bijsturen indien nodig)

// ---------- TIMING ----------
#define MQTT_KEEPALIVE_S   120
#define OLED_UPDATE_MS     5000UL    // OLED refresh elke 5 seconden
#define STATUS_PUB_MS      30000UL   // statuspublicatie interval

// ---------- STATE ----------
WiFiClient   espClient;
PubSubClient client(espClient);
SX1262       radio = new Module(PIN_NSS, PIN_DIO1, PIN_NRST, PIN_BUSY);
U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, OLED_SCL, OLED_SDA, OLED_RST);

static volatile bool pkt_received  = false;
static uint32_t rx_total           = 0;
static uint32_t rx_mqtt_ok         = 0;
static float    last_rssi          = 0;
static float    last_snr           = 0;
static uint32_t last_oled_ms       = 0;
static uint32_t last_status_pub_ms = 0;

// =====================================================
// INTERRUPT — wordt aangeroepen door SX1262 DIO1
// =====================================================
ICACHE_RAM_ATTR void onDio1() {
  pkt_received = true;
}

// =====================================================
// OLED
// =====================================================
void oled_update(const char* status) {
  char regel1[22];
  char regel2[22];
  char regel3[22];

  snprintf(regel1, sizeof(regel1), "%-20s", status);
  snprintf(regel2, sizeof(regel2), "RX:%-4lu MQTT:%-4lu", (unsigned long)rx_total, (unsigned long)rx_mqtt_ok);
  snprintf(regel3, sizeof(regel3), "RSSI:%.0f SNR:%.0f", last_rssi, last_snr);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tf);
  u8g2.drawStr(0, 13, regel1);
  u8g2.drawStr(0, 30, regel2);
  u8g2.drawStr(0, 47, regel3);
  u8g2.sendBuffer();
}

// =====================================================
// WIFI
// =====================================================
void setup_wifi() {
  Serial.printf("\n[WIFI] verbinden met %s\n", ssid);
  oled_update("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 20000) {
      Serial.println("\n[WIFI] timeout, opnieuw proberen");
      WiFi.disconnect(true, true);
      delay(1000);
      WiFi.begin(ssid, password);
      t0 = millis();
    }
  }
  Serial.printf("\n[WIFI] verbonden — IP: %s  RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  oled_update("WiFi OK");
}

// =====================================================
// MQTT
// =====================================================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT IN] %s : ", topic);
  for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
  Serial.println();
}

bool reconnect_mqtt() {
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  Serial.print("[MQTT] verbinden...");
  oled_update("MQTT...");
  String clientId = "Heltec-LoraRx-";
  clientId += String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);
  bool ok = client.connect(clientId.c_str(), mqtt_user, mqtt_pass);
  if (ok) {
    Serial.println(" OK");
    client.subscribe("esp32/test/in");
    oled_update("MQTT OK");
  } else {
    Serial.printf(" FAIL rc=%d\n", client.state());
    oled_update("MQTT FAIL");
  }
  return ok;
}

bool ensure_mqtt() {
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  if (!client.connected()) return reconnect_mqtt();
  client.loop();
  return true;
}

// =====================================================
// STATUS PUBLICATIE
// =====================================================
void publish_status() {
  if (!ensure_mqtt()) return;

  char payload[128];
  snprintf(payload, sizeof(payload),
    "{\"uptime\":%lu,\"rx_total\":%lu,\"rx_mqtt_ok\":%lu,\"rssi_lora\":%.1f,\"wifi_rssi\":%d}",
    (unsigned long)(millis() / 1000),
    (unsigned long)rx_total,
    (unsigned long)rx_mqtt_ok,
    last_rssi,
    (int)WiFi.RSSI()
  );

  bool ok = client.publish(mqtt_status_topic, payload, false);
  Serial.printf("[STATUS] %s → %s\n", ok ? "OK" : "FAIL", payload);
}

// =====================================================
// LORA
// =====================================================
bool setupLoRa() {
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  delay(50);

  int state = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_PWR);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] init MISLUKT code=%d\n", state);
    oled_update("LORA FAIL");
    return false;
  }

  // Interrupt instellen op DIO1
  radio.setDio1Action(onDio1);

  // Ontvangst starten — keert direct terug
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] startReceive MISLUKT code=%d\n", state);
    oled_update("LORA FAIL");
    return false;
  }

  Serial.printf("[LORA] OK  %.1f MHz  BW=%.0f  SF=%u  CR=4/%u  SYNC=0x%02X\n",
                LORA_FREQ, LORA_BW, (unsigned)LORA_SF, (unsigned)LORA_CR, (unsigned)LORA_SYNC);
  oled_update("Luisteren...");
  return true;
}

// =====================================================
// TBAT
// =====================================================
uint32_t read_tbat_mv() {
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    sum += analogReadMilliVolts(TBAT_PIN);
    delay(2);
  }
  float avg_mv = (float)sum / VBAT_SAMPLES;
  return (uint32_t)(avg_mv * TBAT_RATIO * TBAT_CAL + 0.5f);
}

// =====================================================
// HELPERS
// =====================================================
String escapeJson(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== LORA_VV_RECEIVER_MQTT_HELTEC_V2 ===");

  // Vext aan voor OLED
  pinMode(36, OUTPUT);
  digitalWrite(36, LOW);
  delay(100);

  // OLED opstarten
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(50);
  digitalWrite(OLED_RST, HIGH);
  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  oled_update("Opstarten...");

  setup_wifi();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqtt_callback);
  client.setKeepAlive(MQTT_KEEPALIVE_S);
  client.setSocketTimeout(10);
  client.setBufferSize(512);
  reconnect_mqtt();

  if (!setupLoRa()) {
    while (true) delay(1000);
  }

  last_oled_ms       = millis();
  last_status_pub_ms = millis();
}

// =====================================================
// LOOP — blokkeert nooit
// =====================================================
void loop() {

  // --- WiFi + MQTT alive houden ---
  ensure_mqtt();

  // --- Pakket ontvangen via interrupt? ---
  if (pkt_received) {
    pkt_received = false;

    String received;
    int state = radio.readData(received);

    if (state == RADIOLIB_ERR_NONE) {
      last_rssi = radio.getRSSI();
      last_snr  = radio.getSNR();
      rx_total++;

      Serial.println("------ LORA RX ------");
      Serial.println(received);
      Serial.printf("RSSI: %.1f  SNR: %.1f\n", last_rssi, last_snr);

      uint32_t tbat_mv = read_tbat_mv();
      float    hel_temp = temperatureRead();
      Serial.printf("TBAT: %u mV (%.2f V)\n", tbat_mv, tbat_mv / 1000.0f);
      Serial.printf("TEMP: %.1f C\n", hel_temp);

      String payload;
      if (received.startsWith("{") && received.endsWith("}")) {
        payload = received;
        payload.remove(payload.length() - 1);
        payload += ",\"lr\":" + String(last_rssi, 1) + ",\"ls\":" + String(last_snr, 1) +
                   ",\"lnHelTuin-tbat\":" + String(tbat_mv) +
                   ",\"hel_temp\":" + String(hel_temp, 1) + "}";
      } else {
        payload = "{\"raw\":\"" + escapeJson(received) + "\",\"lr\":" +
                  String(last_rssi, 1) + ",\"ls\":" + String(last_snr, 1) +
                  ",\"lnHelTuin-tbat\":" + String(tbat_mv) +
                  ",\"hel_temp\":" + String(hel_temp, 1) + "}";
      }

      if (!ensure_mqtt()) {
        Serial.println("[MQTT OUT] FAIL");
        oled_update("MQTT FAIL");
      } else {
        bool ok = client.publish(mqtt_topic, payload.c_str(), MQTT_RETAIN);
        Serial.printf("[MQTT OUT] %s\n", ok ? "OK" : "FAIL");
        if (ok) rx_mqtt_ok++;
        oled_update(ok ? "RX OK" : "MQTT FAIL");
      }
      Serial.println("---------------------");

    } else {
      Serial.printf("[LORA] readData error=%d\n", state);
    }

    // Ontvangst opnieuw starten na readData
    radio.startReceive();
  }

  // --- OLED periodiek updaten ---
  if (millis() - last_oled_ms >= OLED_UPDATE_MS) {
    last_oled_ms = millis();
    oled_update("Luisteren...");
  }

  // --- Status periodiek publiceren ---
  if (millis() - last_status_pub_ms >= STATUS_PUB_MS) {
    last_status_pub_ms = millis();
    publish_status();
  }

  delay(10);
}
