/*
  SKETCH : LORA_VV_SOIL2_V1
  DEVICE : XIAO ESP32-C3
  ROLE   : SOIL2 / ZTS sender
  NOTES  :
    - fixed working ZTS settings:
      addr=1 baud=4800 func=0x04 start=0x0000 len=2
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ESP32_NOW.h>
#include <Wire.h>
#include <string.h>
#include "esp_sleep.h"
#include <ModbusMaster.h>

#define SKETCH_TAG "VV_SOIL02_V100"

/* ============== CONFIG ============== */
#define ESPNOW_WIFI_CHANNEL 6
static uint8_t AGG_MAC[] = {0xB0,0xA6,0x04,0x07,0xA2,0x80};

#define I2C_SDA   7
#define I2C_SCL   6
#define WAKE_PIN  2

#define INTERVAL_MINUTES 10
#define MINUTE_PHASE     0
#define PHASE_SECOND     16
#define DEV_HOLD_MS      8000UL

#define RS485_RX_PIN     21
#define RS485_TX_PIN     20
#define RS485_DE_RE_PIN  4
#define DE_LOW_IS_RX     1

#define MOSFET_PIN       5
#define MOSFET_ON_HIGH   1

#define ZTS_ADDR         1
#define ZTS_BAUD         4800
#define ZTS_START        0x0000
#define ZTS_LEN          2
#define SENSOR_WARMUP_MS 3200
#define ZTS_ATTEMPTS     5//3
#define RESP_TIMEOUT_MS  900
#define BETWEEN_ATTEMPT_MS 250//150

#define VBAT_PIN        3
#define VBAT_SAMPLES    12
#define VBAT_FACTOR     3.20f
#define VBAT_CAL        1.000f
#define ADC_VREF        3.30f
#define ADC_MAX         4095.0f

#define ACK_WAIT_MS      150
#define MAX_RETRIES      2
/* ==================================== */

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

/* ===== RTC ===== */
static const uint8_t DS3231_ADDR = 0x68;
static uint8_t bcd2dec(uint8_t v){ return (v>>4)*10 + (v&0x0F); }
static uint8_t dec2bcd(uint8_t v){ return ((v/10)<<4) | (v%10); }
static void ds_wreg(uint8_t reg, uint8_t val){
  Wire.beginTransmission(DS3231_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static uint8_t ds_rreg(uint8_t reg){
  Wire.beginTransmission(DS3231_ADDR); Wire.write(reg);
  Wire.endTransmission(false); Wire.requestFrom(DS3231_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}
static bool ds_read_hms(uint8_t &hh, uint8_t &mm, uint8_t &ss){
  Wire.beginTransmission(DS3231_ADDR); Wire.write((uint8_t)0x00);
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
  float frac = ((lsb >> 6) & 0x03) * 0.25f;
  return (float)t + frac;
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
static void ds_set_alarm1_hms_ignore_date(uint8_t hh,uint8_t mm,uint8_t ss){
  ds_wreg(0x07, (dec2bcd(ss)&0x7F));
  ds_wreg(0x08, (dec2bcd(mm)&0x7F));
  ds_wreg(0x09, (dec2bcd(hh)&0x3F));
  ds_wreg(0x0A, 0x80);
  ds_clear_flags();
  ds_enable_a1_int();
  delay(5);
  if (digitalRead(WAKE_PIN) == LOW) ds_clear_flags();
}
static void compute_next_phase_sec(uint8_t hh,uint8_t mm,uint8_t ss,
                                   uint8_t &oh,uint8_t &om,uint8_t &os){
  const int nowMinAbs = (int)hh*60 + (int)mm;
  for(int d=0; d<=24*60; d++){
    int candMinAbs = nowMinAbs + d;
    int ch = (candMinAbs/60)%24;
    int cm = candMinAbs%60;
    int diff = cm - MINUTE_PHASE; if(diff<0) diff += 60;
    if((diff % INTERVAL_MINUTES) != 0) continue;
    if(d==0 && PHASE_SECOND <= ss) continue;
    oh=(uint8_t)ch; om=(uint8_t)cm; os=(uint8_t)PHASE_SECOND; return;
  }
  oh=hh; om=(uint8_t)((mm+INTERVAL_MINUTES)%60); os=(uint8_t)PHASE_SECOND;
}

/* ===== VBAT ===== */
static uint16_t read_vbat_mv(float *dbg_vout=nullptr, uint16_t *dbg_raw=nullptr){
  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_PIN, ADC_11db);
  delay(20);

  uint32_t sum_raw = 0;
  uint32_t sum_mv  = 0;

  for (int i = 0; i < VBAT_SAMPLES; i++) {
    int raw = analogRead(VBAT_PIN);
    int mv  = analogReadMilliVolts(VBAT_PIN);
    sum_raw += raw;
    sum_mv  += mv;
    delay(2);
  }

  float raw_avg = (float)sum_raw / VBAT_SAMPLES;
  float vout    = ((float)sum_mv / VBAT_SAMPLES) / 1000.0f;
  float vbat    = vout * VBAT_FACTOR * VBAT_CAL;

  if (dbg_vout) *dbg_vout = vout;
  if (dbg_raw)  *dbg_raw  = (uint16_t)lroundf(raw_avg);

  return (uint16_t)lroundf(vbat * 1000.0f);
}

/* ===== power + rs485 ===== */
static void rs485_tristate() {
  pinMode(RS485_TX_PIN, INPUT);
  pinMode(RS485_RX_PIN, INPUT);
  pinMode(RS485_DE_RE_PIN, INPUT);
}
static void mosfet_on() {
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, MOSFET_ON_HIGH ? HIGH : LOW);
}
static void mosfet_off() {
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, MOSFET_ON_HIGH ? LOW : HIGH);
  rs485_tristate();
}
static inline void set_tx_mode() {
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, DE_LOW_IS_RX ? HIGH : LOW);
}
static inline void set_rx_mode() {
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, DE_LOW_IS_RX ? LOW : HIGH);
}

/* ===== modbus ===== */
static HardwareSerial RS485(1);
static ModbusMaster node;
static void preTx(){ set_tx_mode(); delayMicroseconds(200); }
static void postTx(){ delayMicroseconds(200); set_rx_mode(); }

static bool read_zts(float &tempC, float &moistPct, uint8_t &err_last, uint32_t &total_ms) {
  tempC = -1.0f; moistPct = -1.0f;
  err_last = 0; total_ms = 0;

  mosfet_on();
  delay(SENSOR_WARMUP_MS);

  bool ok = false;
  for (int attempt = 1; attempt <= ZTS_ATTEMPTS; attempt++) {
    uint32_t t0 = millis();

    RS485.end();
    delay(10);
    RS485.begin(ZTS_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    RS485.setTimeout(RESP_TIMEOUT_MS);

    set_rx_mode();
    delay(25);

    node.begin(ZTS_ADDR, RS485);
    node.preTransmission(preTx);
    node.postTransmission(postTx);

    uint8_t r = node.readInputRegisters(ZTS_START, ZTS_LEN);

    uint32_t dt = millis() - t0;
    total_ms += dt;

    if (r == node.ku8MBSuccess) {
      uint16_t rawM = node.getResponseBuffer(0);
      uint16_t rawT = node.getResponseBuffer(1);
      moistPct = rawM / 10.0f;
      tempC    = (int16_t)rawT / 10.0f;
      Serial.printf("[ZTS] OK  attempt=%d dt=%lums rawM=%u rawT=%u\n",
                    attempt, (unsigned long)dt, (unsigned)rawM, (unsigned)rawT);
      ok = true;
      break;
    } else {
      err_last = r;
      Serial.printf("[ZTS] FAIL attempt=%d dt=%lums err=0x%02X\n",
                    attempt, (unsigned long)dt, (unsigned)r);
      delay(BETWEEN_ATTEMPT_MS);
    }
  }

  RS485.end();
  mosfet_off();
  return ok;
}

/* ===== ESPNOW + ACK ===== */
static volatile bool ack_got=false;
static volatile uint32_t ack_sid=0;
static volatile uint16_t ack_seq=0;

class AggPeer : public ESP_NOW_Peer {
public:
  AggPeer(const uint8_t *mac, uint8_t ch) : ESP_NOW_Peer(mac, ch, WIFI_IF_STA, nullptr) {}
  bool begin(){ return (ESP_NOW.begin() && add()); }
  void onReceive(const uint8_t *d, size_t l, bool) override {
    if (l < sizeof(Ack)) return;
    Ack a; memcpy(&a, d, sizeof(a));
    if (a.ok != 1) return;
    ack_sid = a.session_id;
    ack_seq = a.seq;
    ack_got = true;
  }
  bool send_pkt(const SoilPacket &p){ return send((const uint8_t*)&p, sizeof(p)); }
};
static AggPeer peer(AGG_MAC, ESPNOW_WIFI_CHANNEL);

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
  esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);
  Serial.printf("[SOIL2] gpio_wakeup err=%d\n", (int)err);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup(){
  Serial.begin(115200);
  delay(120);

  Serial.println();
  Serial.println("=================================================");
  Serial.printf("LORA_VV_SOIL2_V1");
  Serial.println("=================================================");

  if (DEV_HOLD_MS > 0) {
    Serial.printf("[DEV] hold %lu ms\n", (unsigned long)DEV_HOLD_MS);
    uint32_t t0 = millis();
    while (millis() - t0 < DEV_HOLD_MS) delay(10);
    Serial.println("[DEV] hold done");
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  pinMode(WAKE_PIN, INPUT_PULLUP);
  ds_config_int_mode();
  ds_clear_flags();

  float vout_dbg=0; uint16_t raw_dbg=0;
  uint16_t vbat_mv = read_vbat_mv(&vout_dbg, &raw_dbg);
  float caseT = ds_read_temp_c();
  Serial.printf("[VBAT] raw_avg=%u vout=%.3fV vbat=%.2fV\n",
                (unsigned)raw_dbg, vout_dbg, vbat_mv/1000.0f);

  float tempC, moistPct;
  uint8_t err_last;
  uint32_t total_ms;
  bool zts_ok = read_zts(tempC, moistPct, err_last, total_ms);
  Serial.printf("[ZTS] total=%lums\n", (unsigned long)total_ms);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
  while(!WiFi.STA.started()) delay(1);

  if (session_id == 0) session_id = esp_random();

  Serial.printf("[SOIL2] MAC=%s wake=%d sid=0x%08lX\n",
                WiFi.macAddress().c_str(), (int)esp_sleep_get_wakeup_cause(), (unsigned long)session_id);

  bool now_ok = peer.begin();
  if (!now_ok) Serial.println("[SOIL2] ESP-NOW init/peer add FAILED");

  bool delivered = false;

  if (now_ok) {
    SoilPacket pkt{};
    pkt.magic = 0xA1;
    pkt.id    = 2;

    pkt.s.session_id = session_id;
    pkt.s.seq        = ++seq;
    pkt.s.t_x10      = zts_ok ? (int16_t)lroundf(tempC * 10.0f) : (int16_t)-10;
    pkt.s.h_x10      = zts_ok ? (uint16_t)lroundf(moistPct * 10.0f) : (uint16_t)0;
    pkt.s.ec_raw     = 0;
    pkt.s.case_x10   = (int16_t)lroundf(caseT * 10.0f);
    pkt.s.vbat_mv    = vbat_mv;
    pkt.s.ph_x10     = 0;
    pkt.s.flags      = zts_ok ? 0x0001 : 0x0000;
    pkt.s.rsv0       = 0;

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
      bool tx  = peer.send_pkt(pkt);
      bool ack = tx ? wait_ack(pkt.s.session_id, pkt.s.seq) : false;

      Serial.printf("[SOIL2] TX seq=%u attempt=%d tx=%d ack=%d T=%.1f M=%.1f caseT=%.1f VBAT=%.2fV flags=0x%04X\n",
                    (unsigned)pkt.s.seq, attempt, tx ? 1 : 0, ack ? 1 : 0,
                    zts_ok ? tempC : -1.0f, zts_ok ? moistPct : -1.0f,
                    caseT, vbat_mv / 1000.0f, (unsigned)pkt.s.flags);

      if (ack) { delivered = true; break; }
      delay(10);
    }
  }

  uint8_t hh, mm, ss;
  if (ds_read_hms(hh, mm, ss)) {
    uint8_t th, tm, ts;
    compute_next_phase_sec(hh, mm, ss, th, tm, ts);
    Serial.printf("[RTC] Now %02u:%02u:%02u -> Alarm %02u:%02u:%02u (delivered=%d)\n",
                  hh, mm, ss, th, tm, ts, delivered ? 1 : 0);
    ds_set_alarm1_hms_ignore_date(th, tm, ts);
    Serial.printf("[RTC] CTRL=0x%02X STATUS=0x%02X INT=%d\n",
                  ds_rreg(0x0E), ds_rreg(0x0F), digitalRead(WAKE_PIN));
  } else {
    Serial.println("[RTC] DS3231 read failed -> timer fallback");
    esp_sleep_enable_timer_wakeup((uint64_t)INTERVAL_MINUTES * 60ULL * 1000000ULL);
  }

  go_sleep_gpio_c3();
}

void loop(){}