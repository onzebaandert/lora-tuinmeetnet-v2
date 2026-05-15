/*
  SKETCH : LORA_VV_SENSORSTATION_V1
  DEVICE : ESP32-C3 Super Mini
  ROLE   : Licht (lux) + temp + vochtigheid + casetemp + batlev naar AGG via ESP-NOW
  SENSOR : BH1750 (I2C 0x23) + SHT3x (I2C 0x44) + DS3231 RTC/temp + LilyGo T-BAT

  Hardware aansluitingen:
    Super Mini GPIO3 (SDA) ──── BH1750 SDA  +  SHT3x SDA  +  DS3231 SDA
    Super Mini GPIO4 (SCL) ──── BH1750 SCL  +  SHT3x SCL  +  DS3231 SCL
    Super Mini GPIO2       ──── DS3231 INT/SQW (wakeup-pin)
    Super Mini GPIO1       ──── T-BAT VBAT spanningsdeler uitgang
                                (100K van bat+ naar GPIO1, 100K van GPIO1 naar GND)
    BH1750 ADDR            ──── GND  (adres 0x23)
    SHT3x  ADDR            ──── GND  (adres 0x44)

  Packet layout (Soil22 struct, id=5):
    t_x10    = SHT3x temperatuur × 10  (°C)
    h_x10    = SHT3x vochtigheid × 10  (%RH)
    ec_raw   = BH1750 lux (uint16, 0–65535)
    case_x10 = DS3231 temp × 10
    vbat_mv  = batterij in mV
    ph_x10   = 0 (ongebruikt)
    flags    = 0x0001 als BH1750 OK | 0x0002 als SHT3x OK

  Libraries:
    - BH1750         door Christopher Laws
    - Adafruit SHT31 door Adafruit (werkt voor SHT30/SHT31/SHT35)
    - Wire           (ingebouwd)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include "esp_sleep.h"
#include <BH1750.h>
#include <Adafruit_SHT31.h>

#define SKETCH_TAG "VV_SENSORSTATION_V1"

/* ============== CONFIG ============== */
#define ESPNOW_WIFI_CHANNEL  6
static const uint8_t AGG_MAC[6] = {0xB0,0xA6,0x04,0x07,0xA2,0x80};

#define I2C_SDA           3    // Super Mini SDA
#define I2C_SCL           4    // Super Mini SCL
#define WAKE_PIN          2    // DS3231 INT/SQW

#define INTERVAL_MINUTES  5
#define MINUTE_PHASE      0
#define PHASE_SECOND      24   // wake op :24 (SOIL1=:08, SOIL2=:16, BH1750=:08)
#define DEV_HOLD_MS       8000UL

#define SLEEP_SEC         30

// Batterij – pas VBAT_RATIO aan na kalibratie
// Verbind bat+ via 100K/100K spanningsdeler met GPIO1
#define VBAT_PIN          1
#define VBAT_SAMPLES      12
#define VBAT_RATIO        2.0f   // 100K/100K deler; aanpassen na meting

#define ACK_WAIT_MS       150
#define MAX_RETRIES       2
/* ==================================== */

/* ===== packet struct (zelfde als andere nodes) ===== */
#pragma pack(push, 1)
struct __attribute__((packed)) Soil22 {
  uint32_t session_id;
  uint16_t seq;
  int16_t  t_x10;
  uint16_t h_x10;
  uint16_t ec_raw;    // BH1750 lux
  int16_t  case_x10;  // DS3231 temp × 10
  uint16_t vbat_mv;
  uint16_t ph_x10;
  uint16_t flags;
  uint16_t rsv0;
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

/* ===== DS3231 ===== */
static const uint8_t DS3231_ADDR = 0x68;
static uint8_t bcd2dec(uint8_t v){ return (v>>4)*10 + (v&0x0F); }
static uint8_t dec2bcd(uint8_t v){ return ((v/10)<<4) | (v%10); }

static void ds_wreg(uint8_t reg, uint8_t val){
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t ds_rreg(uint8_t reg){
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(DS3231_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}
static bool ds_read_hms(uint8_t &hh, uint8_t &mm, uint8_t &ss){
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
static float ds_read_temp_c(){
  uint8_t msb = ds_rreg(0x11);
  uint8_t lsb = ds_rreg(0x12);
  int8_t t = (int8_t)msb;
  return (float)t + ((lsb >> 6) & 0x03) * 0.25f;
}
static void ds_clear_flags(){
  uint8_t st = ds_rreg(0x0F);
  st &= ~(uint8_t)0x03;
  ds_wreg(0x0F, st);
}
static void ds_config_int_mode(){
  uint8_t ctrl = ds_rreg(0x0E);
  ctrl |= (1<<2); ctrl &= ~(1<<1); ctrl &= ~(1<<0);
  ds_wreg(0x0E, ctrl);
}
static void ds_enable_a1_int(){
  uint8_t ctrl = ds_rreg(0x0E);
  ctrl |= (1<<2); ctrl |= (1<<0);
  ds_wreg(0x0E, ctrl);
}
static void ds_set_alarm1_hms_ignore_date(uint8_t hh, uint8_t mm, uint8_t ss){
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
                                   uint8_t &oh, uint8_t &om, uint8_t &os){
  const int nowMinAbs = (int)hh*60 + (int)mm;
  for (int d = 0; d <= 24*60; d++) {
    int candMinAbs = nowMinAbs + d;
    int ch = (candMinAbs/60) % 24;
    int cm = candMinAbs % 60;
    int diff = cm - MINUTE_PHASE; if (diff < 0) diff += 60;
    if ((diff % INTERVAL_MINUTES) != 0) continue;
    if (d == 0 && PHASE_SECOND <= ss) continue;
    oh=(uint8_t)ch; om=(uint8_t)cm; os=(uint8_t)PHASE_SECOND; return;
  }
  oh=hh; om=(uint8_t)((mm+INTERVAL_MINUTES)%60); os=(uint8_t)PHASE_SECOND;
}

/* ===== VBAT ===== */
static uint16_t read_vbat_mv(){
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) { sum += analogReadMilliVolts(VBAT_PIN); delay(2); }
  float avg_mv = (float)sum / (float)VBAT_SAMPLES;
  return (uint16_t)(avg_mv * VBAT_RATIO + 0.5f);
}

/* ===== ESP-NOW ACK ===== */
static volatile bool     ack_got = false;
static volatile uint32_t ack_sid = 0;
static volatile uint16_t ack_seq = 0;

static void on_ack_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len){
  (void)info;
  if (!data || len < (int)sizeof(Ack)) return;
  Ack a; memcpy(&a, data, sizeof(a));
  if (a.ok != 1) return;
  ack_sid = a.session_id; ack_seq = a.seq; ack_got = true;
}
static bool wait_ack(uint32_t sid, uint16_t seqv){
  ack_got = false;
  uint32_t t0 = millis();
  while (millis() - t0 < ACK_WAIT_MS) {
    if (ack_got && ack_sid == sid && ack_seq == seqv) return true;
    delay(1);
  }
  return false;
}

/* ===== retained ===== */
RTC_DATA_ATTR static uint32_t session_id = 0;
RTC_DATA_ATTR static uint16_t seq = 0;

static void go_sleep_gpio_c3(){
  pinMode(WAKE_PIN, INPUT_PULLUP);
  uint64_t mask = 1ULL << WAKE_PIN;
  esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_deep_sleep_start();
}

/* ===== SETUP ===== */
void setup(){
  Serial.begin(115200);
  delay(120);
  Serial.println();
  Serial.println("=================================================");
  Serial.println(SKETCH_TAG);
  Serial.println("=================================================");

  if (DEV_HOLD_MS > 0) {
    uint32_t t0 = millis();
    while (millis() - t0 < DEV_HOLD_MS) delay(10);
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(WAKE_PIN, INPUT_PULLUP);
  ds_config_int_mode();
  ds_clear_flags();

  // BH1750 lezen
  BH1750 lightMeter;
  bool bh_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  delay(180); // eerste meting ~120 ms
  float lux = bh_ok ? lightMeter.readLightLevel() : -1.0f;
  uint16_t lux_u16 = (bh_ok && lux >= 0) ? (uint16_t)min((float)65535, lux) : 0;
  Serial.printf("[BH1750] ok=%d lux=%.1f\n", bh_ok ? 1 : 0, lux);

  // SHT3x lezen
  Adafruit_SHT31 sht3x;
  bool sht_ok = sht3x.begin(0x44);
  float sht_temp = sht_ok ? sht3x.readTemperature() : NAN;
  float sht_hum  = sht_ok ? sht3x.readHumidity()    : NAN;
  if (!sht_ok || isnan(sht_temp)) { sht_ok = false; sht_temp = 0.0f; sht_hum = 0.0f; }
  Serial.printf("[SHT3x]  ok=%d T=%.1f H=%.1f\n", sht_ok ? 1 : 0, sht_temp, sht_hum);

  float    caseT   = ds_read_temp_c();
  uint16_t vbat_mv = read_vbat_mv();
  Serial.printf("[DS3231] caseT=%.2f°C\n", caseT);
  Serial.printf("[VBAT]   %.2fV  (VBAT_RATIO=%.2f — aanpassen na kalibratie)\n",
                vbat_mv / 1000.0f, VBAT_RATIO);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  if (session_id == 0) session_id = esp_random();

  Serial.printf("[STNST] MAC=%s sid=0x%08lX\n",
                WiFi.macAddress().c_str(), (unsigned long)session_id);

  bool delivered = false;

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init FAILED");
  } else {
    esp_now_register_recv_cb(on_ack_recv);

    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, AGG_MAC, 6);
    p.channel = ESPNOW_WIFI_CHANNEL;
    p.encrypt = false;
    esp_err_t e = esp_now_add_peer(&p);
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST)
      Serial.printf("[ESPNOW] add_peer FAILED err=%d\n", (int)e);

    uint16_t flags = 0;
    if (bh_ok)  flags |= 0x0001;
    if (sht_ok) flags |= 0x0002;

    SoilPacket pkt{};
    pkt.magic        = 0xA1;
    pkt.id           = 5;   // sensorstation
    pkt.s.session_id = session_id;
    pkt.s.seq        = ++seq;
    pkt.s.t_x10      = (int16_t)lroundf(sht_temp * 10.0f);
    pkt.s.h_x10      = (uint16_t)lroundf(sht_hum  * 10.0f);
    pkt.s.ec_raw     = lux_u16;
    pkt.s.case_x10   = (int16_t)lroundf(caseT * 10.0f);
    pkt.s.vbat_mv    = vbat_mv;
    pkt.s.ph_x10     = 0;
    pkt.s.flags      = flags;
    pkt.s.rsv0       = 0;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
      esp_err_t txe = esp_now_send(AGG_MAC, (const uint8_t*)&pkt, sizeof(pkt));
      bool tx_ok  = (txe == ESP_OK);
      bool ack_ok = tx_ok ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;

      Serial.printf("[TX] seq=%u attempt=%d tx=%d ack=%d lux=%u T=%.1f H=%.1f caseT=%.1f vbat=%.2fV flags=0x%04X\n",
                    (unsigned)pkt.s.seq, attempt, tx_ok?1:0, ack_ok?1:0,
                    (unsigned)lux_u16, sht_temp, sht_hum, caseT,
                    vbat_mv/1000.0f, (unsigned)flags);

      if (ack_ok) { delivered = true; break; }
      delay(10);
    }
  }

  Serial.printf("[SLEEP] %d sec\n", SLEEP_SEC);
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SEC * 1000000ULL);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_deep_sleep_start();
}

void loop(){}
