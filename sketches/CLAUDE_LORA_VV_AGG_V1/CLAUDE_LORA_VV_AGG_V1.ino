/*
  SKETCH : LORA_VV_AGG_V1
  DEVICE : XIAO ESP32-C3
  ROLE   : Aggregator
  NOTES  :
    - Receives SoilPacket (28 bytes) from SOIL nodes
    - ACKs sender
    - Stores SOIL->AGG RSSI in Soil22.rsv0 (low byte, int8)
    - Builds fixed 97-byte uplink for meshnode
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include "esp_sleep.h"
#include <Preferences.h>

/* ================= CONFIG ================= */
#define ESPNOW_WIFI_CHANNEL 6

// DS3231
#define I2C_SDA   6
#define I2C_SCL   7
#define WAKE_PIN  2

// cycle
#define INTERVAL_MINUTES 5
#define MINUTE_PHASE     0
#define WAKE_SECOND      5
#define LISTEN_MS        30000UL

// TTN NODE MAC (unicast)  <-- UPDATED
static const uint8_t MESH_MAC[6] = {0xB0,0xA6,0x04,0x05,0xD4,0x48};//TTN NODE

// known soil nodes  (slot 0=SOIL1, 1=SOIL2, 2=FLOW, 3=BH1750)
// Na het flashen van LORA_VV_WATERFLOW_V1: lees MAC uit seriële output
// "[WF] MAC=XX:XX:XX:XX:XX:XX" en vul in bij FLOW hieronder.
static const char *SOIL_IDS[4] = {"SOIL1","SOIL2","FLOW","BH1750"};
static const uint8_t SOIL_MACS[4][6] = {
  {0xB0,0xA6,0x04,0x04,0xC0,0x98}, // SOIL1
  {0xB0,0xA6,0x04,0x04,0xF2,0x6C}, // SOIL2
  {0x80,0xF1,0xB2,0x64,0x48,0xDC}, // FLOW (waterflow XIAO - update MAC na flashen!)
  {0xE8,0xF6,0x0A,0x14,0x09,0x28}  // BH1750
};

// VBAT
#define VBAT_PIN        A1
#define VBAT_SAMPLES    12
#define VBAT_FACTOR     3.20f
#define VBAT_CAL        0.957f
#define ADC_VREF        3.30f
#define ADC_MAX         4095.0f
/* ========================================== */

#pragma pack(push, 1)
struct __attribute__((packed)) Soil22 {
  uint32_t session_id;
  uint16_t seq;
  int16_t  t_x10;
  uint16_t h_x10;
  uint16_t ec_raw;
  int16_t  case_x10;
  uint16_t vbat_mv;
  uint16_t ph_x10;
  uint16_t flags;
  uint16_t rsv0;      // low byte used for RSSI (int8)
};
#pragma pack(pop)
static_assert(sizeof(Soil22) == 22, "Soil22 size");

#pragma pack(push, 1)
struct __attribute__((packed)) SoilPacket {
  uint8_t magic;
  uint8_t id;
  Soil22  s;
  uint8_t pad[4];
};
#pragma pack(pop)
static_assert(sizeof(SoilPacket) == 28, "SoilPacket size");

#pragma pack(push, 1)
struct __attribute__((packed)) Ack {
  uint32_t session_id;
  uint16_t seq;
  uint8_t  ok;
  uint8_t  pad;
};
#pragma pack(pop)
static_assert(sizeof(Ack) == 8, "Ack size");

#pragma pack(push, 1)
struct __attribute__((packed)) AggUplink97 {
  uint32_t agg_sid;
  uint32_t agg_seq;
  uint8_t  count;
  Soil22   items[5];  // 0=self, 1..4 soils (SOIL1, SOIL2, FLOW, BH1750)
};
#pragma pack(pop)
static_assert(sizeof(AggUplink97) == 119, "AggUplink97 size");

/* ===== RTC ===== */
static const uint8_t DS3231_ADDR = 0x68;
static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static void ds_wreg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t ds_rreg(uint8_t reg) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(DS3231_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}
static bool ds_read_hms(uint8_t &hh, uint8_t &mm, uint8_t &ss) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(DS3231_ADDR, (uint8_t)3);
  if (Wire.available() < 3) return false;
  ss = bcd2dec(Wire.read() & 0x7F);
  mm = bcd2dec(Wire.read() & 0x7F);
  hh = bcd2dec(Wire.read() & 0x3F);
  return true;
}
static float ds_read_temp_c() {
  uint8_t msb = ds_rreg(0x11);
  uint8_t lsb = ds_rreg(0x12);
  int8_t t = (int8_t)msb;
  float frac = ((lsb >> 6) & 0x03) * 0.25f;
  return (float)t + frac;
}
static void ds_clear_flags() {
  uint8_t st = ds_rreg(0x0F);
  st &= ~(uint8_t)0x03;
  ds_wreg(0x0F, st);
}
static void ds_config_int_mode() {
  uint8_t ctrl = ds_rreg(0x0E);
  ctrl |=  (1 << 2);
  ctrl &= ~(1 << 1);
  ctrl &= ~(1 << 0);
  ds_wreg(0x0E, ctrl);
}
static void ds_enable_a1_int() {
  uint8_t ctrl = ds_rreg(0x0E);
  ctrl |= (1 << 2);
  ctrl |= (1 << 0);
  ds_wreg(0x0E, ctrl);
}
static void ds_set_alarm1_hms_ignore_date(uint8_t hh, uint8_t mm, uint8_t ss) {
  ds_wreg(0x07, (dec2bcd(ss) & 0x7F));
  ds_wreg(0x08, (dec2bcd(mm) & 0x7F));
  ds_wreg(0x09, (dec2bcd(hh) & 0x3F));
  ds_wreg(0x0A, 0x80);
  ds_clear_flags();
  ds_enable_a1_int();
  delay(5);
  if (digitalRead(WAKE_PIN) == LOW) ds_clear_flags();
}
static void compute_next_phase_sec(uint8_t hh, uint8_t mm, uint8_t ss,
                                   uint8_t &oh, uint8_t &om, uint8_t &os) {
  const int nowMinAbs = (int)hh * 60 + (int)mm;
  for (int d = 0; d <= (24 * 60); d++) {
    const int candMinAbs = nowMinAbs + d;
    const int ch = (candMinAbs / 60) % 24;
    const int cm = candMinAbs % 60;
    int diff = cm - MINUTE_PHASE; if (diff < 0) diff += 60;
    if ((diff % INTERVAL_MINUTES) != 0) continue;
    if (d == 0 && WAKE_SECOND <= ss) continue;
    oh = (uint8_t)ch; om = (uint8_t)cm; os = (uint8_t)WAKE_SECOND;
    return;
  }
  oh = hh; om = (uint8_t)((mm + INTERVAL_MINUTES) % 60); os = (uint8_t)WAKE_SECOND;
}

/* ===== helpers ===== */
static bool mac_eq(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }
static void mac_to_str(char out[18], const uint8_t mac[6]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static uint16_t read_vbat_mv() {
  analogReadResolution(12);
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) { sum += analogRead(VBAT_PIN); delay(2); }
  float raw = (float)sum / (float)VBAT_SAMPLES;
  float v = (raw / ADC_MAX) * ADC_VREF * VBAT_FACTOR * VBAT_CAL;
  return (uint16_t)lroundf(v * 1000.0f);
}

/* ===== runtime ===== */
static Soil22 soil_last[4];
static bool soil_have[4] = {false,false,false,false};
static uint32_t rx_ok = 0, rx_drop = 0, ack_sent = 0;

RTC_DATA_ATTR static uint32_t agg_sid = 0;
static uint32_t agg_seq = 0;

/* ===== RX callback ===== */
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len < (int)sizeof(SoilPacket)) { rx_drop++; return; }

  SoilPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != 0xA1) { rx_drop++; return; }

  int idx = -1;
  for (int i = 0; i < 4; i++) {
    if (mac_eq(info->src_addr, SOIL_MACS[i])) { idx = i; break; }
  }
  if (idx < 0) { rx_drop++; return; }

  int8_t rssi = 0;
  if (info->rx_ctrl) rssi = (int8_t)info->rx_ctrl->rssi;

  soil_last[idx] = p.s;
  soil_last[idx].rsv0 = (uint16_t)(uint8_t)rssi;
  soil_have[idx] = true;
  rx_ok++;

  char macs[18];
  mac_to_str(macs, info->src_addr);
  Serial.printf("[RX] %s mac=%s seq=%u T=%.1f H=%.1f EC=%u case=%.1f vbat=%umV RSSI=%d\n",
                SOIL_IDS[idx], macs, (unsigned)p.s.seq,
                p.s.t_x10 / 10.0f, p.s.h_x10 / 10.0f, (unsigned)p.s.ec_raw,
                p.s.case_x10 / 10.0f, (unsigned)p.s.vbat_mv, (int)rssi);

  Ack a{};
  a.session_id = p.s.session_id;
  a.seq = p.s.seq;
  a.ok = 1;
  esp_now_send(info->src_addr, (const uint8_t*)&a, sizeof(a));
  ack_sent++;
}

/* ===== sleep ===== */
static void go_sleep_gpio_c3() {
  pinMode(WAKE_PIN, INPUT_PULLUP);
  uint64_t mask = 1ULL << WAKE_PIN;
  esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  Serial.printf("[AGG] gpio_wakeup err=%d\n", (int)err);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(120);

  Serial.println();
  Serial.println("===========================================");
  Serial.println("SKETCH : LORA_VV_AGG_V1");
  Serial.printf("CH=%d WAKE_SECOND=%d LISTEN_MS=%lums\n",
                ESPNOW_WIFI_CHANNEL, WAKE_SECOND, (unsigned long)LISTEN_MS);
  Serial.println("===========================================");

  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(WAKE_PIN, INPUT_PULLUP);
  ds_config_int_mode();
  ds_clear_flags();

  uint16_t self_vbat = read_vbat_mv();
  float self_caseT = ds_read_temp_c();
  Serial.printf("[AGG] self VBAT=%.2fV (%umV) caseT=%.2fC\n",
                self_vbat / 1000.0f, (unsigned)self_vbat, self_caseT);

  memset(soil_last, 0, sizeof(soil_last));
  soil_have[0] = soil_have[1] = soil_have[2] = soil_have[3] = false;
  rx_ok = rx_drop = ack_sent = 0;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  if (agg_sid == 0) agg_sid = esp_random();

  Preferences prefs;
  prefs.begin("agg", false);
  agg_seq = prefs.getUInt("seq", 0);
  agg_seq++;
  prefs.putUInt("seq", agg_seq);
  prefs.end();

  Serial.printf("[AGG] MAC=%s sid=0x%08lX agg_seq=%lu wake=%d\n",
                WiFi.macAddress().c_str(),
                (unsigned long)agg_sid,
                (unsigned long)agg_seq,
                (int)esp_sleep_get_wakeup_cause());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[AGG] esp_now_init FAILED");
  } else {
    esp_now_register_recv_cb(on_recv);

    for (int i = 0; i < 4; i++) {
      esp_now_peer_info_t p{};
      memcpy(p.peer_addr, SOIL_MACS[i], 6);
      p.channel = ESPNOW_WIFI_CHANNEL;
      p.encrypt = false;
      esp_now_add_peer(&p);
    }
    esp_now_peer_info_t m{};
    memcpy(m.peer_addr, MESH_MAC, 6);
    m.channel = ESPNOW_WIFI_CHANNEL;
    m.encrypt = false;
    esp_now_add_peer(&m);
  }

  Serial.printf("[AGG] Listening... %lums\n", (unsigned long)LISTEN_MS);
  uint32_t t0 = millis();
  while (millis() - t0 < LISTEN_MS) delay(5);

  Serial.printf("[AGG] Listen done. rx_ok=%lu rx_drop=%lu ack=%lu have1=%d have2=%d have3=%d have4=%d\n",
                (unsigned long)rx_ok, (unsigned long)rx_drop, (unsigned long)ack_sent,
                soil_have[0]?1:0, soil_have[1]?1:0, soil_have[2]?1:0, soil_have[3]?1:0);

  AggUplink97 u{};
  memset(&u, 0, sizeof(u));
  u.agg_sid = agg_sid;
  u.agg_seq = agg_seq;

  u.items[0].session_id = agg_sid;
  u.items[0].seq        = (uint16_t)(u.agg_seq & 0xFFFF);
  u.items[0].case_x10   = (int16_t)lroundf(self_caseT * 10.0f);
  u.items[0].vbat_mv    = self_vbat;
  u.items[0].flags      = 0xA001;
  u.items[0].rsv0       = 0;

  uint8_t count = 1;
  for (int i = 0; i < 4; i++) {
    if (soil_have[i]) {
      u.items[count] = soil_last[i];
      // low byte = RSSI (already set), high byte = sensor slot (1-based) for receiver
      u.items[count].rsv0 = (soil_last[i].rsv0 & 0x00FF) | ((uint16_t)(i + 1) << 8);
      count++;
    }
  }
  u.count = count;

  bool uplink_ok = false;
for (int k = 0; k < 8; k++) {
  esp_err_t su = esp_now_send(MESH_MAC, (const uint8_t*)&u, sizeof(u));
  Serial.printf("[AGG] Uplink->MESH try=%d ok=%d len=%u count=%u agg_seq=%lu\n",
                k + 1,
                (su == ESP_OK) ? 1 : 0,
                (unsigned)sizeof(u),
                (unsigned)u.count,
                (unsigned long)u.agg_seq);

  if (su == ESP_OK) {
    uplink_ok = true;
  }

  delay(250);
}
  uint8_t hh, mm, ss;
  if (ds_read_hms(hh, mm, ss)) {
    uint8_t th, tm, ts;
    compute_next_phase_sec(hh, mm, ss, th, tm, ts);
    Serial.printf("[RTC] Now %02u:%02u:%02u -> Alarm %02u:%02u:%02u\n", hh, mm, ss, th, tm, ts);
    ds_set_alarm1_hms_ignore_date(th, tm, ts);
    Serial.printf("[RTC] CTRL=0x%02X STATUS=0x%02X INT=%d\n",
                  ds_rreg(0x0E), ds_rreg(0x0F), digitalRead(WAKE_PIN));
  } else {
    Serial.println("[RTC] DS3231 read failed -> timer fallback");
    esp_sleep_enable_timer_wakeup((uint64_t)INTERVAL_MINUTES * 60ULL * 1000000ULL);
  }

  go_sleep_gpio_c3();
}

void loop() {}