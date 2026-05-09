/*
  SKETCH : LORA_VV_BH1750_V1
  DEVICE : XIAO ESP32-C3
  ROLE   : Lichtmeting (lux) + casetemp + batlev naar AGG via ESP-NOW
  SENSOR : BH1750 (I2C 0x23) + DS3231 RTC/temp + LilyGo T-BAT

  Hardware aansluitingen:
    XIAO SDA (D4/GPIO6) ──── BH1750 SDA, DS3231 SDA
    XIAO SCL (D5/GPIO7) ──── BH1750 SCL, DS3231 SCL
    XIAO A1  (GPIO3)    ──── T-BAT VBAT divider
    XIAO D2  (GPIO2)    ──── DS3231 INT/SQW (wakeup)
    BH1750 ADDR         ──── GND (adres 0x23)

  Packet layout (Soil22 struct, id=4):
    ec_raw   = lux (uint16, 0–65535)
    case_x10 = DS3231 temp * 10
    vbat_mv  = batterij in mV
    t/h/ph   = 0 (ongebruikt)
    flags    = 0x0001 als BH1750 OK

  Libraries: BH1750 (Christopher Laws), Wire (ingebouwd)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include "esp_sleep.h"
#include <BH1750.h>

#define SKETCH_TAG "VV_BH1750_V1"

/* ============== CONFIG ============== */
#define ESPNOW_WIFI_CHANNEL  6
static const uint8_t AGG_MAC[6] = {0xB0,0xA6,0x04,0x07,0xA2,0x80};

#define I2C_SDA           6
#define I2C_SCL           7
#define WAKE_PIN          2

#define INTERVAL_MINUTES  5
#define MINUTE_PHASE      0
#define PHASE_SECOND      8
#define DEV_HOLD_MS       8000UL

#define VBAT_PIN          4    // GPIO4 = D2
#define VBAT_SAMPLES      12
#define VBAT_FACTOR       3.20f
#define VBAT_CAL          0.957f
#define ADC_VREF          3.30f
#define ADC_MAX           4095.0f

#define ACK_WAIT_MS       150
#define MAX_RETRIES       2
/* ==================================== */

/* ===== packet struct (zelfde als SOIL1) ===== */
#pragma pack(push, 1)
struct __attribute__((packed)) Soil22 {
  uint32_t session_id;
  uint16_t seq;
  int16_t  t_x10;
  uint16_t h_x10;
  uint16_t ec_raw;     // lux waarde
  int16_t  case_x10;  // DS3231 temp * 10
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
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  delay(20);
  uint32_t sum = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) { sum += analogRead(VBAT_PIN); delay(2); }
  float raw  = (float)sum / (float)VBAT_SAMPLES;
  float vout = (raw / ADC_MAX) * ADC_VREF;
  float vbat = vout * VBAT_FACTOR * VBAT_CAL;
  return (uint16_t)lroundf(vbat * 1000.0f);
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
  delay(180); // BH1750 eerste meting ~120ms
  float lux = bh_ok ? lightMeter.readLightLevel() : -1.0f;
  uint16_t lux_u16 = (bh_ok && lux >= 0) ? (uint16_t)min((float)65535, lux) : 0;
  Serial.printf("[BH1750] ok=%d lux=%.1f\n", bh_ok ? 1 : 0, lux);

  float caseT    = ds_read_temp_c();
  uint16_t vbat_mv = read_vbat_mv();
  Serial.printf("[DS3231] caseT=%.2f°C\n", caseT);
  Serial.printf("[VBAT]   %.2fV\n", vbat_mv / 1000.0f);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while (!WiFi.STA.started()) delay(1);

  if (session_id == 0) session_id = esp_random();

  Serial.printf("[BH1750] MAC=%s sid=0x%08lX\n",
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

    SoilPacket pkt{};
    pkt.magic        = 0xA1;
    pkt.id           = 4;          // BH1750 node
    pkt.s.session_id = session_id;
    pkt.s.seq        = ++seq;
    pkt.s.t_x10      = 0;
    pkt.s.h_x10      = 0;
    pkt.s.ec_raw     = lux_u16;
    pkt.s.case_x10   = (int16_t)lroundf(caseT * 10.0f);
    pkt.s.vbat_mv    = vbat_mv;
    pkt.s.ph_x10     = 0;
    pkt.s.flags      = bh_ok ? 0x0001 : 0x0000;
    pkt.s.rsv0       = 0;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
      esp_err_t txe = esp_now_send(AGG_MAC, (const uint8_t*)&pkt, sizeof(pkt));
      bool tx_ok  = (txe == ESP_OK);
      bool ack_ok = tx_ok ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;

      Serial.printf("[TX] seq=%u attempt=%d tx=%d ack=%d lux=%u caseT=%.1f vbat=%.2fV flags=0x%04X\n",
                    (unsigned)pkt.s.seq, attempt, tx_ok?1:0, ack_ok?1:0,
                    (unsigned)lux_u16, caseT, vbat_mv/1000.0f, (unsigned)pkt.s.flags);

      if (ack_ok) { delivered = true; break; }
      delay(10);
    }
  }

  uint8_t hh, mm, ss;
  if (ds_read_hms(hh, mm, ss)) {
    uint8_t th, tm, ts;
    compute_next_phase_sec(hh, mm, ss, th, tm, ts);
    Serial.printf("[RTC] Now %02u:%02u:%02u -> Alarm %02u:%02u:%02u (delivered=%d)\n",
                  hh, mm, ss, th, tm, ts, delivered?1:0);
    ds_set_alarm1_hms_ignore_date(th, tm, ts);
  } else {
    Serial.println("[RTC] DS3231 read failed -> timer fallback");
    esp_sleep_enable_timer_wakeup((uint64_t)INTERVAL_MINUTES * 60ULL * 1000000ULL);
  }

  go_sleep_gpio_c3();
}

void loop(){}
